#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <rtl-sdr.h>

namespace openitc {

struct RtlConfig {
    int device_index = 0;
    uint32_t center_freq = 220137500;  // ITC channel 102 (national common)
    uint32_t sample_rate = 960000;
    int gain_tenths_db = 402;  // 40.2 dB; 0 => AGC
    int ppm = 0;
    bool bias_tee = true;
    bool agc = false;
};

std::string rtl_device_name(int index);
int rtl_device_count();

// Base frequency of the 220 MHz ITC band; channel spacing is 25 kHz.
constexpr uint32_t itc_frequency(uint32_t channel) {
    return 217587500u + channel * 25000u;
}

class RtlSource {
public:
    explicit RtlSource(const RtlConfig& cfg);
    ~RtlSource();

    RtlSource(const RtlSource&) = delete;
    RtlSource& operator=(const RtlSource&) = delete;

    void open_device();
    void configure();

    // Blocking read of `count` interleaved uint8 I/Q pairs (2*count bytes).
    void read(std::vector<uint8_t>& buffer, size_t count);

    const RtlConfig& config() const { return cfg_; }

private:
    RtlConfig cfg_;
    rtlsdr_dev_t* dev_ = nullptr;
};

}  // namespace openitc
