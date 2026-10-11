// OpenITC decoder.
//
// Native capture + NCO channelization (with bias tee). The PHY (demod + FEC)
// currently uses the reference `s2p` engine fed over UDP; decoded packets are
// read back over ZeroMQ. Includes a continuous mode so sparse beacons
// (type 0x02) can be caught as they transmit.
//
// See docs/reverse-engineering.md for the native demod/FEC work-in-progress.

#include "decode.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <zmq.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "dsp.h"
#include "rtl_source.h"

namespace openitc {
namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

void fail(const char* msg) {
    std::fprintf(stderr, "error: %s\n", msg);
    std::exit(1);
}

void write_be16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v & 0xff);
}

void decode_beacon(const uint8_t* p, size_t n) {
    if (n < 21) {
        std::printf("  beacon payload too short (%zu)\n", n);
        return;
    }
    uint32_t radio = (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    int32_t lat_raw = (int32_t)((uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) |
                                (uint32_t(p[6]) << 8) | p[7]);
    int32_t lon_raw = (int32_t)((uint32_t(p[8]) << 24) | (uint32_t(p[9]) << 16) |
                                (uint32_t(p[10]) << 8) | p[11]);
    double lat = lat_raw / 600000.0;
    double lon = lon_raw / 600000.0;
    if (lat < 21 || lat > 70 || lon < -170 || lon > -50) {
        std::printf("  non-US beacon radio=0x%06X lat=%.6f lon=%.6f\n", radio, lat, lon);
        return;
    }
    std::printf(
        "BEACON radio=0x%06X lat=%.6f lon=%.6f ch=%u rate=%u util=%u flag1=0x%02x "
        "ch_list=[%u,%u,%u,%u,%u,%u]\n",
        radio, lat, lon, p[12], p[13], p[14], p[15], p[16], p[17], p[18], p[19],
        p[20], p[21]);
}

struct Options {
    openitc::RtlConfig cfg;
    size_t nsamples = 960000;
    long channel = 102;
    std::string mode = "HR";
    std::string s2p_path = "C:\\Users\\Rose\\Documents\\itcmon-v10\\s2p.exe";
    int udp_port = 17700;
    bool loop = false;
    bool beacons_only = false;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) fail("missing argument");
            return argv[++i];
        };
        if (a == "-c") o.channel = std::atol(next());
        else if (a == "-s") o.cfg.sample_rate = (uint32_t)std::strtoul(next(), nullptr, 10);
        else if (a == "-g") {
            double g = std::atof(next());
            o.cfg.agc = g <= 0; o.cfg.gain_tenths_db = (int)std::lround(g * 10);
        } else if (a == "-e") o.cfg.ppm = std::atoi(next());
        else if (a == "-b") o.cfg.bias_tee = std::atoi(next()) != 0;
        else if (a == "-n") o.nsamples = std::strtoull(next(), nullptr, 10);
        else if (a == "-m") o.mode = next();
        else if (a == "--s2p") o.s2p_path = next();
        else if (a == "--udp") o.udp_port = std::atoi(next());
        else if (a == "--loop") o.loop = true;
        else if (a == "-q") o.beacons_only = true;
    }
    return o;
}

}  // namespace

int run_decode(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::signal(SIGINT, on_sigint);

    Options o = parse(argc, argv);
    const uint32_t base_rate = (o.mode == "FR") ? 192000u : 96000u;
    const double chan_freq = openitc::itc_frequency((uint32_t)o.channel);
    o.cfg.center_freq = (uint32_t)chan_freq + 100000;  // dodge RTL DC spike

    std::printf("OpenITC decode: ch %ld  mode %s  baseband %u Hz%s\n", o.channel,
                o.mode.c_str(), base_rate, o.loop ? "  (continuous)" : "");

    // Spawn the reference s2p on its own UDP/ZMQ ports, once.
    const int zmq_port = o.udp_port + 1000;
    std::string cmd = "\"" + o.s2p_path + "\" -r " + std::to_string(o.udp_port) +
                      " -m " + o.mode;
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cwd = o.s2p_path.substr(0, o.s2p_path.find_last_of("\\/"));
    std::vector<char> mut(cmd.begin(), cmd.end()); mut.push_back(0);
    if (!CreateProcessA(nullptr, mut.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
        std::fprintf(stderr, "failed to start s2p (%lu) at %s\n", GetLastError(),
                     o.s2p_path.c_str());
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    void* zctx = zmq_ctx_new();
    void* sub = zmq_socket(zctx, ZMQ_SUB);
    std::string ep = "tcp://127.0.0.1:" + std::to_string(zmq_port);
    zmq_connect(sub, ep.c_str());
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "", 0);
    int rcvto = 100; zmq_setsockopt(sub, ZMQ_RCVTIMEO, &rcvto, sizeof(rcvto));

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET us = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons((u_short)o.udp_port);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    openitc::RtlSource src(o.cfg);
    src.open_device();
    src.configure();

    const int CHUNK = 511;
    const int BURST = 4000;
    const int factor = (int)(o.cfg.sample_rate / base_rate);
    int total_pkts = 0;

    do {
        std::vector<uint8_t> iq;
        src.read(iq, o.nsamples);
        std::vector<openitc::cf> x = openitc::to_baseband(iq);
        openitc::mix_shift(x, chan_freq - o.cfg.center_freq, o.cfg.sample_rate);
        openitc::FirDecimator dec(factor, 0.45 * base_rate / o.cfg.sample_rate,
                                  16 * factor + 1);
        std::vector<openitc::cf> y;
        dec.process(x, y);
        for (auto& s : y) s *= 0.001f;  // MCR amplitude convention

        uint8_t buf[4 + CHUNK * 8];
        for (size_t base = 0; base + 16 < y.size(); base += BURST) {
            size_t bend = std::min(y.size(), base + (size_t)BURST);
            uint16_t seq = 0;
            for (size_t off = base; off < bend; off += CHUNK) {
                size_t cnt = std::min((size_t)CHUNK, bend - off);
                bool last = (off + cnt >= bend);
                buf[0] = last ? 1 : 0; buf[1] = 0; write_be16(buf + 2, seq);
                std::memcpy(buf + 4, &y[off], cnt * sizeof(openitc::cf));
                sendto(us, (const char*)buf, (int)(4 + cnt * 8), 0,
                       (sockaddr*)&dst, sizeof(dst));
                seq++;
                std::this_thread::sleep_for(std::chrono::microseconds(
                    (long long)(cnt * 1e6 / base_rate)));
            }
            for (;;) {
                uint8_t msg[512];
                int n = zmq_recv(sub, msg, sizeof(msg), 0);
                if (n < 0) break;
                if (n >= 5 && msg[0] == 0x49) {
                    total_pkts++;
                    if (msg[3] == 0x02) decode_beacon(msg + 5, (size_t)(n - 5));
                    else if (!o.beacons_only)
                        std::printf("packet type=0x%02x len=%d\n", msg[3], n - 5);
                }
            }
        }
        // short drain
        for (int t = 0; t < 5; ++t) {
            uint8_t msg[512];
            int n = zmq_recv(sub, msg, sizeof(msg), 0);
            if (n >= 5 && msg[0] == 0x49) {
                total_pkts++;
                if (msg[3] == 0x02) decode_beacon(msg + 5, (size_t)(n - 5));
                else if (!o.beacons_only) std::printf("packet type=0x%02x len=%d\n", msg[3], n - 5);
            }
        }
        if (!o.loop) break;
    } while (!g_stop);

    std::printf("total packets: %d\n", total_pkts);
    zmq_close(sub); zmq_ctx_term(zctx);
    closesocket(us); WSACleanup();
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return 0;
}

}  // namespace openitc
