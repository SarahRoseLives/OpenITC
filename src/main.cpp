#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "dsp.h"
#include "rtl_source.h"
#include "decode.h"
#include "native.h"

namespace {

void usage() {
    std::printf(
        "OpenITC - console RTL-SDR receiver for ITC (220 MHz PTC)\n"
        "\n"
        "Usage: openitc [options]\n"
        "  -d <index>   RTL device index              (default 0)\n"
        "  -f <hz>      center frequency in Hz        (default 220137500 = ch 102)\n"
        "  -c <channel> ITC channel (freq = 217587500 + ch*25000)\n"
        "  -s <rate>    sample rate in Hz             (default 960000)\n"
        "  -g <db>      tuner gain in dB, 0 for AGC   (default 40.2)\n"
        "  -e <ppm>     frequency correction in ppm   (default 0)\n"
        "  -b <0|1>     bias tee                      (default 1 = on)\n"
        "  -n <n>       number of I/Q samples to read (default 960000)\n"
        "  -R <rate>    channelize: mix target channel to DC and decimate to\n"
        "               this baseband rate (e.g. 96000 for HR); output is\n"
        "               interleaved complex float32 (the s2p input format)\n"
        "  -o <file>    write captured I/Q to file\n"
        "  -l           list devices and exit\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "decode") {
        return openitc::run_decode(argc, argv);
    }
    if (argc > 1 && std::string(argv[1]) == "demod") {
        return openitc::run_demod(argc, argv);
    }
    if (argc > 1 && std::string(argv[1]) == "monitor") {
        return openitc::run_monitor(argc, argv);
    }
    openitc::RtlConfig cfg;
    size_t nsamples = 960000;
    std::string outfile;
    uint32_t out_rate = 0;
    long channel = -1;
    double out_scale = 0.001;  // matches MCR's baseband amplitude convention

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](void) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "-l") {
            int n = openitc::rtl_device_count();
            std::printf("Found %d device(s):\n", n);
            for (int d = 0; d < n; ++d) {
                std::printf("  %d: %s\n", d, openitc::rtl_device_name(d).c_str());
            }
            return 0;
        } else if (a == "-d") {
            cfg.device_index = std::atoi(next());
        } else if (a == "-f") {
            cfg.center_freq = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
        } else if (a == "-c") {
            channel = std::atol(next());
        } else if (a == "-s") {
            cfg.sample_rate = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
        } else if (a == "-g") {
            double g = std::atof(next());
            cfg.agc = (g <= 0.0);
            cfg.gain_tenths_db = static_cast<int>(std::lround(g * 10.0));
        } else if (a == "-e") {
            cfg.ppm = std::atoi(next());
        } else if (a == "-b") {
            cfg.bias_tee = (std::atoi(next()) != 0);
        } else if (a == "-n") {
            nsamples = static_cast<size_t>(std::strtoull(next(), nullptr, 10));
        } else if (a == "-R") {
            out_rate = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
        } else if (a == "-G") {
            out_scale = std::atof(next());
        } else if (a == "-o") {
            outfile = next();
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    double chan_freq = cfg.center_freq;
    if (channel >= 0) {
        chan_freq = openitc::itc_frequency(static_cast<uint32_t>(channel));
        if (out_rate) {
            // Offset the tuner from the channel to dodge the RTL DC spike.
            cfg.center_freq = static_cast<uint32_t>(chan_freq) + 100000;
        } else {
            cfg.center_freq = static_cast<uint32_t>(chan_freq);
        }
    }

    try {
        std::printf("OpenITC - device %d (%s)\n", cfg.device_index,
                    openitc::rtl_device_name(cfg.device_index).c_str());
        std::printf("  center = %.3f MHz  rate = %u Hz  gain = %s  ppm = %d  bias_tee = %s\n",
                    cfg.center_freq / 1e6, cfg.sample_rate,
                    cfg.agc ? "AGC"
                            : (std::to_string(cfg.gain_tenths_db / 10.0) + " dB").c_str(),
                    cfg.ppm, cfg.bias_tee ? "on" : "off");

        openitc::RtlSource src(cfg);
        src.open_device();
        src.configure();

        std::vector<uint8_t> iq;
        src.read(iq, nsamples);
        const size_t complexes = iq.size() / 2;
        std::printf("  read %zu complex samples (%.3f s)\n", complexes,
                    complexes / static_cast<double>(cfg.sample_rate));

        if (out_rate == 0) {
            double sum_i = 0, sum_q = 0, sum_p = 0;
            for (size_t k = 0; k < complexes; ++k) {
                double i = double(iq[2 * k]) - 127.5;
                double q = double(iq[2 * k + 1]) - 127.5;
                sum_i += i; sum_q += q; sum_p += i * i + q * q;
            }
            double n = double(complexes);
            std::printf("  DC = (%.2f, %.2f)  mean power = %.2f dB\n", n ? sum_i / n : 0,
                        n ? sum_q / n : 0, 10 * std::log10(sum_p / (n ? n : 1) + 1e-12));
            if (!outfile.empty()) {
                FILE* f = std::fopen(outfile.c_str(), "wb");
                if (!f) { std::fprintf(stderr, "cannot open %s\n", outfile.c_str()); return 1; }
                std::fwrite(iq.data(), 1, iq.size(), f);
                std::fclose(f);
                std::printf("  wrote %zu bytes to %s\n", iq.size(), outfile.c_str());
            }
            return 0;
        }

        // Channelize: mix target channel to DC, then FIR-decimate.
        if (cfg.sample_rate % out_rate != 0) {
            std::fprintf(stderr, "sample rate %u not an integer multiple of %u\n",
                         cfg.sample_rate, out_rate);
            return 1;
        }
        int factor = static_cast<int>(cfg.sample_rate / out_rate);

        std::vector<openitc::cf> x = openitc::to_baseband(iq);
        openitc::mix_shift(x, chan_freq - cfg.center_freq, cfg.sample_rate);

        openitc::FirDecimator dec(factor, 0.45 * out_rate / cfg.sample_rate,
                                  16 * factor + 1);
        std::vector<openitc::cf> y;
        dec.process(x, y);

        for (auto& s : y) s *= static_cast<float>(out_scale);

        double p = 0;
        for (auto& s : y) p += std::norm(s);
        std::printf("  channelized %s to %u Hz: %zu complex samples (%.3f s), power %.2f dB\n",
                    channel >= 0 ? ("ch" + std::to_string(channel)).c_str() : "band",
                    out_rate, y.size(), y.size() / double(out_rate),
                    10 * std::log10(p / (y.empty() ? 1 : double(y.size())) + 1e-12));

        if (!outfile.empty()) {
            FILE* f = std::fopen(outfile.c_str(), "wb");
            if (!f) { std::fprintf(stderr, "cannot open %s\n", outfile.c_str()); return 1; }
            std::fwrite(y.data(), sizeof(openitc::cf), y.size(), f);
            std::fclose(f);
            std::printf("  wrote %zu complex-float samples to %s\n", y.size(),
                        outfile.c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
