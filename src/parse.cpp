#include "parse.h"

namespace openitc {
namespace {

int32_t be32(const uint8_t* p) {
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | p[3]);
}

int rev5(int v) {
    int r = 0;
    for (int i = 0; i < 5; ++i) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

}  // namespace

void parse_event(PacketEvent& e) {
    const std::vector<uint8_t>& p = e.payload;
    switch (e.type) {
        case 0x02: {
            if (p.size() < 22) return;
            BeaconInfo& b = e.beacon;
            b.flags = p[0];
            b.radio_id = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
            b.lat = be32(&p[4]) / 600000.0;
            b.lon = be32(&p[8]) / 600000.0;
            b.channel = p[12];
            b.rate = p[13];
            b.util = p[14];
            b.flag1 = p[15];
            for (int i = 0; i < 6; ++i) b.ch_list[i] = p[16 + i];
            b.valid = true;
            break;
        }
        case 0x06: {
            if (p.size() < 9) return;
            TimeInfo& t = e.time;
            t.tid = tid24(p);
            t.year = 2000 + p[3];
            t.month = p[4];
            t.day = p[5];
            t.hour = p[6];
            t.minute = p[7];
            t.second = p[8];
            t.valid = true;
            break;
        }
        case 0x05: {
            if (p.size() < 4) return;
            CntlInfo& c = e.cntl;
            c.tid = tid24(p);
            c.seq = p[3];
            c.data.assign(p.begin() + 4, p.end());
            c.valid = true;
            break;
        }
        case 0x33:
        case 0x34: {
            if (p.size() < 3) return;
            WiuInfo& w = e.wiu;
            w.wiu_id = tid24(p);
            w.indicators.assign(p.begin() + 3, p.end());
            const size_t n = w.indicators.size();
            if (n * 8 >= 17) {
                const uint8_t* d = w.indicators.data();
                size_t bit = 0;
                auto read_bits = [&](int k) {
                    int v = 0;
                    for (int i = 0; i < k; ++i) {
                        const size_t bi = bit >> 3;
                        const int b = (bi < n) ? ((d[bi] >> (7 - (bit & 7))) & 1) : 0;
                        v = (v << 1) | b;
                        ++bit;
                    }
                    return v;
                };
                w.sw = read_bits(2) & 3;
                w.sig[0] = rev5(read_bits(5));
                w.sig[1] = rev5(read_bits(5));
                w.sig[2] = rev5(read_bits(5));
                w.has_typical = true;
            }
            w.valid = true;
            break;
        }
        default:
            break;
    }
}

}  // namespace openitc
