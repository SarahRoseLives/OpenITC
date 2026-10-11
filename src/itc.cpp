#include "itc.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace openitc {

void pi4dqpsk_demod(const std::vector<cf>& baseband, int sps, int offset,
                    std::vector<uint8_t>& bits) {
    bits.clear();
    if (sps < 1) return;
    const int n = static_cast<int>(baseband.size());
    if (offset < 0 || offset >= sps) return;
    int prev = -1;
    for (int i = offset; i < n; i += sps) {
        if (prev < 0) {
            prev = i;
            continue;
        }
        // Differential phase between consecutive symbol samples. This sign
        // convention matches s2p / OpenITC's channelizer output directly.
        const cf d = std::conj(baseband[i]) * baseband[prev];
        double ph = std::atan2(d.imag(), d.real());
        // wrap to (-pi, pi]
        ph -= 2.0 * M_PI * std::floor((ph + M_PI) / (2.0 * M_PI));
        // 4 decision regions -> 2 bits (b0 = uVar18, b1 = uVar19)
        uint8_t b0, b1;
        if (ph <= 0.0) {
            b0 = 0;
            b1 = (ph <= -M_PI / 2.0) ? 1 : 0;
        } else {
            b0 = 1;
            b1 = (ph > M_PI / 2.0) ? 1 : 0;
        }
        bits.push_back(b0);
        bits.push_back(b1);
        prev = i;
    }
}

std::vector<SyncHit> find_sync(const std::vector<uint8_t>& bits, int max_errors) {
    std::vector<SyncHit> hits;
    const size_t n = bits.size();
    if (n < kUniqueWordLen) return hits;
    // Precompute forward and reversed UW.
    uint8_t uw[64], uwr[64];
    for (int i = 0; i < kUniqueWordLen; ++i) {
        uw[i] = kUniqueWordBits[i] - '0';
        uwr[i] = kUniqueWordBits[kUniqueWordLen - 1 - i] - '0';
    }
    int last = -1;
    for (size_t p = 0; p + kUniqueWordLen <= n; ++p) {
        int e = 0, er = 0;
        for (int i = 0; i < kUniqueWordLen; ++i) {
            uint8_t b = bits[p + i];
            if (b != uw[i]) ++e;
            if (b != uwr[i]) ++er;
        }
        int best = e < er ? e : er;
        if (best <= max_errors) {
            if (static_cast<int>(p) - last < kUniqueWordLen) continue;  // dedupe
            hits.push_back({p, best});
            last = static_cast<int>(p);
        }
    }
    return hits;
}

std::vector<uint8_t> pack_bits_msb(const std::vector<uint8_t>& bits, size_t start,
                                   size_t nbits) {
    std::vector<uint8_t> out;
    uint8_t acc = 0;
    int cnt = 0;
    for (size_t i = 0; i < nbits; ++i) {
        if (start + i >= bits.size()) break;
        acc = static_cast<uint8_t>((acc << 1) | (bits[start + i] & 1));
        if (++cnt == 8) {
            out.push_back(acc);
            acc = 0;
            cnt = 0;
        }
    }
    if (cnt) out.push_back(static_cast<uint8_t>(acc << (8 - cnt)));
    return out;
}

namespace {

// Output tables of s2p's header FEC (dumped from s2p at 0x10040b000/0x10040b080).
// Equivalent to even parity over the tap masks below (K=7, rate 1/2).
constexpr uint8_t kTapA = 0x6d;  // -> 0x10040b000
constexpr uint8_t kTapB = 0x4f;  // -> 0x10040b080

int parity8(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

// Viterbi decode of 48 coded bits (24 steps x 2 bits) -> 24 info bits.
// index = (state<<1)|u, nextstate = index & 0x3f, branch metric = #mismatches.
void viterbi24(const int r[48], uint8_t out[24]) {
    const int INF = 1 << 20;
    int pm[64];
    for (int i = 0; i < 64; ++i) pm[i] = (i == 0) ? 0 : INF;
    int pred[24][64];
    for (int t = 0; t < 24; ++t) {
        int npm[64];
        for (int i = 0; i < 64; ++i) npm[i] = INF;
        for (int i = 0; i < 64; ++i) pred[t][i] = -1;
        const int r0 = r[2 * t], r1 = r[2 * t + 1];
        for (int st = 0; st < 64; ++st) {
            if (pm[st] >= INF) continue;
            for (int u = 0; u < 2; ++u) {
                const int idx = (st << 1) | u;
                const int ns = idx & 0x3f;
                const int bm = (parity8((uint8_t)idx & kTapA) ^ r0) +
                               (parity8((uint8_t)idx & kTapB) ^ r1);
                const int c = pm[st] + bm;
                if (c < npm[ns]) { npm[ns] = c; pred[t][ns] = st; }
            }
        }
        for (int i = 0; i < 64; ++i) pm[i] = npm[i];
    }
    int st = 0;
    for (int i = 1; i < 64; ++i)
        if (pm[i] < pm[st]) st = i;
    for (int t = 23; t >= 0; --t) {
        out[t] = (uint8_t)(st & 1);
        st = pred[t][st];
    }
}

}  // namespace

namespace {

uint8_t g_exp[512];
uint8_t g_log[256];
struct GfInit {
    GfInit() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            g_exp[i] = (uint8_t)x;
            g_log[x] = (uint8_t)i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11d;
        }
        for (int i = 255; i < 512; ++i) g_exp[i] = g_exp[i - 255];
    }
} g_gfinit;

inline uint8_t gmul(uint8_t a, uint8_t b) {
    if (!a || !b) return 0;
    return g_exp[g_log[a] + g_log[b]];
}
inline uint8_t ginv(uint8_t a) { return g_exp[255 - g_log[a]]; }

// Reed-Solomon decode of one 16-byte block (GF(2^8), poly 0x11d, fcr=0,
// prim=1, nroots=4) -> RS(16,12). Returns #errors, or -1 if uncorrectable.
int rs_decode(uint8_t* r, int n) {
    const int nroots = 4;
    uint8_t S[4];
    for (int j = 0; j < nroots; ++j) {
        uint8_t acc = 0;
        for (int i = 0; i < n; ++i)
            if (r[i]) acc ^= gmul(r[i], g_exp[(j * (n - 1 - i)) % 255]);
        S[j] = acc;
    }
    bool zero = true;
    for (int j = 0; j < nroots; ++j) if (S[j]) zero = false;
    if (zero) return 0;

    uint8_t C[4] = {1, 0, 0, 0}, B[4] = {1, 0, 0, 0};
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int nn = 0; nn < nroots; ++nn) {
        uint8_t d = S[nn];
        for (int i = 1; i <= L; ++i) d ^= gmul(C[i], S[nn - i]);
        if (!d) { ++m; continue; }
        uint8_t T[4];
        for (int i = 0; i < 4; ++i) T[i] = C[i];
        const uint8_t coef = gmul(d, ginv(b));
        for (int i = 0; i < nroots - m; ++i) C[i + m] ^= gmul(coef, B[i]);
        if (2 * L <= nn) { L = nn - L + 1; for (int i = 0; i < 4; ++i) B[i] = T[i]; b = d; m = 1; }
        else ++m;
    }
    if (L > nroots / 2) return -1;

    int errpos[16], ne = 0;
    for (int i = 0; i < n; ++i) {
        const int li = n - 1 - i;
        uint8_t acc = 0;
        for (int j = 0; j <= L; ++j) acc ^= gmul(C[j], g_exp[((255 - li) * j) % 255]);
        if (!acc) errpos[ne++] = i;
    }
    if (ne != L) return -1;

    uint8_t omega[4] = {0, 0, 0, 0};
    for (int i = 0; i < nroots; ++i) {
        uint8_t acc = 0;
        for (int j = 0; j <= i && j <= L; ++j) acc ^= gmul(C[j], S[i - j]);
        omega[i] = acc;
    }
    for (int k = 0; k < ne; ++k) {
        const int pos = errpos[k];
        const int li = n - 1 - pos;
        const uint8_t Xinv = g_exp[(255 - li) % 255];
        uint8_t num = 0;
        for (int i = 0; i < nroots; ++i) num ^= gmul(omega[i], g_exp[(i * g_log[Xinv]) % 255]);
        uint8_t den = 0;
        for (int j = 1; j <= L; ++j)
            if (j & 1) den ^= gmul(C[j], g_exp[((j - 1) * g_log[Xinv]) % 255]);
        if (den) r[pos] ^= gmul(gmul(g_exp[li], num), ginv(den));
    }
    return L;
}

}  // namespace

bool decode_header(const uint8_t in6[6], ItcHeader& out) {
    int bits[48];
    for (int k = 0; k < 48; ++k)
        bits[k] = (in6[k >> 3] >> (7 - (k & 7))) & 1;
    uint8_t hdr[24];
    viterbi24(bits, hdr);
    int V = 0;
    for (int i = 0; i < 24; ++i) V = (V << 1) | (hdr[i] & 1);

    // 3-bit header check: LFSR-reduce bits 23..3 with poly 0xb.
    int v = V;
    for (int b = 0x17; b >= 3; --b)
        if ((v >> b) & 1) v ^= 0xb << (b - 3);
    const bool check_ok = ((v & 7) == 0);

    out.length = (V >> 15) & 0x1ff;
    out.mode = (V >> 11) & 0xf;
    out.type = (V >> 3) & 0xff;
    const int hn = out.type >> 4;
    const bool type_ok = (out.type != 0) && (hn <= 7) && (((0x56 >> hn) & 1) == 0);
    out.valid = check_ok && type_ok && (out.length <= 0xc0) && (out.mode == 1);
    return out.valid;
}

bool decode_packet(const std::vector<uint8_t>& packed, ItcPacket& out) {
    out = ItcPacket{};
    if (packed.size() < 14) return false;
    ItcHeader h;
    if (!decode_header(&packed[8], h)) return false;

    const int L = h.length;
    const int B = (L + 12) / 12;   // number of 16-byte RS blocks
    const int ebp = L + 4 * B;     // total interleaved bytes
    if (L <= 0 || B <= 0 || B > 64 || ebp <= 0) return false;
    if (packed.size() < (size_t)(14 + ebp)) return false;

    const uint8_t* data = &packed[14];
    // Block de-interleave: for edi=0..15, b=0..B-1, position p=edi+16*b.
    std::vector<uint8_t> c78((size_t)B * 16, 0);
    int k = 0;
    for (int edi = 0; edi < 16; ++edi)
        for (int b = 0; b < B; ++b) {
            const int p = edi + 16 * b;
            if (p < ebp) c78[p] = data[k++];
        }
    // s2p relocates the last block's parity: move the final 4 interleaved bytes
    // to the end of the last RS block (and zero the source).
    for (int j = 0; j < 4; ++j) {
        c78[(size_t)B * 16 - 1 - j] = c78[(size_t)ebp - 1 - j];
        c78[(size_t)ebp - 1 - j] = 0;
    }

    std::vector<uint8_t> a78;
    a78.reserve((size_t)B * 12);
    for (int b = 0; b < B; ++b) {
        uint8_t* blk = &c78[(size_t)b * 16];
        if (rs_decode(blk, 16) < 0) return false;
        for (int i = 0; i < 12; ++i) a78.push_back(blk[i]);
    }

    // CRC-16/ARC over the first L bytes (last 2 of which are the CRC).
    uint16_t crc = 0;
    for (int i = 0; i < L; ++i) {
        crc ^= a78[i];
        for (int b = 0; b < 8; ++b)
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1) ? 0xA001 : 0));
    }
    if (crc != 0) return false;

    int plen;
    if (h.type == 0x70) plen = L - 2;              // unicast (special format)
    else if (h.type > 5) { if (L < 6) return false; plen = L - 6; }
    else plen = L - 2;
    if (plen < 0) return false;

    out.type = h.type;
    out.payload.assign(a78.begin(), a78.begin() + plen);
    out.valid = true;
    return true;
}

}  // namespace openitc
