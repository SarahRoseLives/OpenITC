// Native (no s2p) demodulator + sync detector — work in progress toward the
// full open-source PHY. M2: pi/4-DQPSK differential demod + 64-bit sync.

#include "native.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "dsp.h"
#include "engine.h"
#include "itc.h"
#include "model.h"
#include "repository.h"
#include "rtl_source.h"

namespace openitc {
namespace {

void fail(const char* m) { std::fprintf(stderr, "error: %s\n", m); std::exit(1); }

void demod_usage() {
    std::printf(
        "Usage: openitc demod [options]\n"
        "  Decode one ITC channel with the native PHY (demod + FEC).\n"
        "  -c <channel>  ITC channel                    (default 142)\n"
        "  -m <HR|FR>    rate                           (default FR)\n"
        "  -f <file>     decode an offline baseband capture (complex float32)\n"
        "                instead of capturing live\n"
        "  -s <rate>     capture sample rate in Hz      (default 960000)\n"
        "  -g <db>       tuner gain, 0 for AGC          (default 40.2)\n"
        "  -e <ppm>      frequency correction           (default 0)\n"
        "  -b <0|1>      bias tee                       (default 1 = on)\n"
        "  -n <n>        number of I/Q samples to read  (default 960000)\n"
        "  -E <n>        max sync bit errors            (default 8)\n"
        "  -h            show this help and exit\n");
}

void monitor_usage() {
    std::printf(
        "Usage: openitc monitor [options]\n"
        "  Scan every PTC channel from one wideband capture and list packets\n"
        "  as they arrive, until interrupted (Ctrl-C).\n"
        "  -d <index>    RTL device index               (default 0)\n"
        "  -s <rate>     capture rate in Hz, multiple of 192000 (default 2304000)\n"
        "  -g <db>       tuner gain, 0 for AGC          (default 42.0)\n"
        "  -e <ppm>      frequency correction           (default 0)\n"
        "  -b <0|1>      bias tee                       (default 1 = on)\n"
        "  -t <ms>       capture chunk size in ms       (default 250)\n"
        "  -C <s>        per-packet repeat cooldown (s) (default 8)\n"
        "  -E <n>        max sync bit errors            (default 8)\n"
        "  -h            show this help and exit\n");
}

}  // namespace

std::vector<cf> capture_channel(long channel, const std::string& mode,
                                const RtlConfig& cfg_in, size_t nsamples) {
    RtlConfig cfg = cfg_in;
    const uint32_t base_rate = (mode == "FR") ? 192000u : 96000u;
    const double chan_freq = itc_frequency((uint32_t)channel);
    cfg.center_freq = (uint32_t)(chan_freq + 100000.0);
    RtlSource src(cfg);
    src.open_device();
    src.configure();
    std::vector<uint8_t> iq;
    src.read(iq, nsamples);
    std::vector<cf> x = to_baseband(iq);
    mix_shift(x, chan_freq - cfg.center_freq, cfg.sample_rate);
    int factor = (int)(cfg.sample_rate / base_rate);
    FirDecimator dec(factor, 0.45 * base_rate / cfg.sample_rate, 16 * factor + 1);
    std::vector<cf> y;
    dec.process(x, y);
    return y;
}

int run_demod(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    RtlConfig cfg;
    size_t nsamples = 960000;
    long channel = 142;
    std::string mode = "FR";
    std::string file;
    int max_errors = 8;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) fail("missing argument");
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { demod_usage(); return 0; }
        else if (a == "-c") channel = std::atol(next());
        else if (a == "-s") cfg.sample_rate = (uint32_t)std::strtoul(next(), nullptr, 10);
        else if (a == "-g") {
            double g = std::atof(next());
            cfg.agc = g <= 0; cfg.gain_tenths_db = (int)std::lround(g * 10);
        } else if (a == "-e") cfg.ppm = std::atoi(next());
        else if (a == "-b") cfg.bias_tee = std::atoi(next()) != 0;
        else if (a == "-n") nsamples = std::strtoull(next(), nullptr, 10);
        else if (a == "-m") mode = next();
        else if (a == "-E") max_errors = std::atoi(next());
        else if (a == "-f") file = next();
    }

    const uint32_t base_rate = (mode == "FR") ? 192000u : 96000u;
    std::printf("OpenITC native demod: ch %ld  mode %s  baseband %u Hz\n", channel,
                mode.c_str(), base_rate);
    std::vector<cf> y;
    if (!file.empty()) {
        std::ifstream in(file, std::ios::binary);
        if (!in) fail("cannot open baseband file");
        float buf[4096];
        while (in.read(reinterpret_cast<char*>(buf), sizeof(buf)) || in.gcount() > 0) {
            const std::streamsize n = in.gcount() / (std::streamsize)sizeof(float);
            for (std::streamsize k = 0; k + 1 < n; k += 2)
                y.push_back(cf(buf[k], buf[k + 1]));
        }
        std::printf("  loaded %zu baseband samples from %s\n", y.size(), file.c_str());
    } else {
        y = capture_channel(channel, mode, cfg, nsamples);
    }
    std::printf("  %zu baseband samples (%.1f s)\n", y.size(),
                y.size() / double(base_rate));

    const int sps = 12;
    int total = 0, decoded = 0;
    for (int o = 0; o < sps; ++o) {
        std::vector<uint8_t> bits;
        pi4dqpsk_demod(y, sps, o, bits);
        auto hits = find_sync(bits, max_errors);
        for (auto h : hits) {
            auto bytes = pack_bits_msb(bits, h.bit_pos, kUniqueWordLen + 6 * 8 + 256 * 8);
            if (bytes.size() < 14) continue;
            ItcHeader hdr;
            const bool ok = decode_header(&bytes[8], hdr);
            std::printf("offset %2d bit %6zu err %d  hdr=%02x%02x%02x  len=%3d mode=%d type=%02x %s\n",
                        o, h.bit_pos, h.errors, bytes[8], bytes[9], bytes[10], hdr.length,
                        hdr.mode, hdr.type, ok ? "OK" : "--");
            if (!ok) continue;
            ++decoded;
            ItcPacket pkt;
            if (decode_packet(bytes, pkt) && pkt.valid) {
                std::printf("      PACKET type=%02x len=%zu\n", pkt.type, pkt.payload.size());
                if (pkt.type == 0x02 && pkt.payload.size() >= 22) {
                    const auto& p = pkt.payload;
                    const uint32_t rid = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
                    const int32_t lat = (int32_t)(((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                                                  ((uint32_t)p[6] << 8) | p[7]);
                    const int32_t lon = (int32_t)(((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) |
                                                  ((uint32_t)p[10] << 8) | p[11]);
                    std::printf("      BEACON id=%06x lat=%.6f lon=%.6f ch=%d rate=%d util=%d "
                                "list=%d,%d,%d,%d,%d,%d\n",
                                rid, lat / 600000.0, lon / 600000.0, p[12], p[13], p[14],
                                p[16], p[17], p[18], p[19], p[20], p[21]);
                }
            }
        }
        total += (int)hits.size();
    }
    std::printf("total sync hits: %d, headers decoded: %d\n", total, decoded);
    return 0;
}

namespace {

void print_hex(const std::vector<uint8_t>& p, size_t from, size_t maxn) {
    for (size_t i = from; i < p.size() && i < from + maxn; ++i) std::printf("%02x", p[i]);
}

void print_wiu(const WiuInfo& w) {
    std::printf("  WIU=%06x data=", w.wiu_id);
    print_hex(w.indicators, 0, 16);
    if (w.has_typical) {
        static const char kSw[5] = "INRX";
        std::printf("  [typical] sw=%c sig=%d,%d,%d", kSw[w.sw & 3], w.sig[0], w.sig[1],
                    w.sig[2]);
    }
    std::printf("\n");
}

void print_event(const PacketEvent& e) {
    char ts[16];
    std::time_t t = (std::time_t)e.t_unix;
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
    const auto& p = e.payload;
    std::printf("[%s] ch%03d %s  type=%02x %-7s len=%zu", ts, e.channel, mode_name(e.mode),
                e.type, type_name(e.type), p.size());
    if (e.type == 0x02 && e.beacon.valid) {
        const BeaconInfo& b = e.beacon;
        std::printf("  id=%06x lat=%.6f lon=%.6f ch=%d rate=%d util=%d list=%d,%d,%d,%d,%d,%d",
                    b.radio_id, b.lat, b.lon, b.channel, b.rate, b.util, b.ch_list[0],
                    b.ch_list[1], b.ch_list[2], b.ch_list[3], b.ch_list[4], b.ch_list[5]);
    } else if (e.type == 0x06 && e.time.valid) {
        const TimeInfo& ti = e.time;
        std::printf("  TID=%06x  %04d-%02d-%02d %02d:%02d:%02d UTC", ti.tid, ti.year, ti.month,
                    ti.day, ti.hour, ti.minute, ti.second);
    } else if (e.type == 0x05 && e.cntl.valid) {
        std::printf("  TID=%06x seq=%d data=", e.cntl.tid, e.cntl.seq);
        print_hex(e.cntl.data, 0, 16);
    } else if ((e.type == 0x33 || e.type == 0x34) && e.wiu.valid) {
        print_wiu(e.wiu);
    } else if (p.size() >= 3) {
        std::printf("  TID=%06x  ", tid24(p));
        print_hex(p, 3, 32);
    } else {
        std::printf("  ");
        print_hex(p, 0, 32);
    }
    std::printf("\n");
}

}  // namespace

int run_monitor(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    EngineConfig ec;
    ec.rtl.sample_rate = 2304000;
    ec.rtl.center_freq = 221050000;  // midpoint of the 27 PTC channels
    ec.rtl.bias_tee = true;
    ec.rtl.gain_tenths_db = 420;
    ec.chunk_ms = 250;
    ec.max_errors = 8;
    double cooldown = 8.0;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) fail("missing argument");
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { monitor_usage(); return 0; }
        else if (a == "-d") ec.rtl.device_index = std::atoi(next());
        else if (a == "-s") ec.rtl.sample_rate = (uint32_t)std::strtoul(next(), nullptr, 10);
        else if (a == "-g") {
            double g = std::atof(next());
            ec.rtl.agc = g <= 0; ec.rtl.gain_tenths_db = (int)std::lround(g * 10);
        } else if (a == "-e") ec.rtl.ppm = std::atoi(next());
        else if (a == "-b") ec.rtl.bias_tee = std::atoi(next()) != 0;
        else if (a == "-t") ec.chunk_ms = std::atoi(next());
        else if (a == "-E") ec.max_errors = std::atoi(next());
        else if (a == "-C") cooldown = std::atof(next());
    }
    if (ec.channels.empty()) ec.channels = default_channels();

    MonitorEngine eng(ec);
    Repository repo;
    std::map<std::string, std::chrono::steady_clock::time_point> last_seen;
    eng.set_callback([&](const PacketEvent& e) {
        repo.observe(e);
        std::string key((const char*)e.payload.data(), e.payload.size());
        key.push_back((char)e.type);
        key.push_back((char)e.channel);
        auto now = std::chrono::steady_clock::now();
        auto it = last_seen.find(key);
        if (it != last_seen.end() &&
            std::chrono::duration<double>(now - it->second).count() < cooldown)
            return;
        last_seen[key] = now;
        print_event(e);
    });

    std::printf("OpenITC monitor - device %d (%s)\n", ec.rtl.device_index,
                eng.device_name().c_str());
    const double span = (itc_frequency(176) - itc_frequency(101)) / 1e6;
    std::printf("  %zu channels, center %.3f MHz, span %.3f MHz, rate %u Hz, gain %s,"
                " bias_tee %s\n",
                ec.channels.size(), ec.rtl.center_freq / 1e6, span, ec.rtl.sample_rate,
                ec.rtl.agc ? "AGC"
                           : (std::to_string(ec.rtl.gain_tenths_db / 10.0) + " dB").c_str(),
                ec.rtl.bias_tee ? "on" : "off");
    std::printf("  scanning... Ctrl-C to stop\n");

    if (!eng.start()) {
        std::fprintf(stderr, "error: %s\n", eng.last_error().c_str());
        return 1;
    }
    while (eng.running())
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!eng.last_error().empty())
        std::fprintf(stderr, "error: %s\n", eng.last_error().c_str());
    return 0;
}

}  // namespace openitc
