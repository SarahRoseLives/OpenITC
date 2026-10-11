#include "rtl_source.h"

#include <stdexcept>

namespace openitc {

int rtl_device_count() {
    return rtlsdr_get_device_count();
}

std::string rtl_device_name(int index) {
    const char* name = rtlsdr_get_device_name(index);
    return name ? name : "(unknown)";
}

RtlSource::RtlSource(const RtlConfig& cfg) : cfg_(cfg) {}

RtlSource::~RtlSource() {
    if (dev_) {
        rtlsdr_close(dev_);
        dev_ = nullptr;
    }
}

void RtlSource::open_device() {
    if (rtlsdr_open(&dev_, cfg_.device_index) != 0 || dev_ == nullptr) {
        throw std::runtime_error("failed to open RTL-SDR device " +
                                 std::to_string(cfg_.device_index));
    }
}

void RtlSource::configure() {
    if (!dev_) {
        throw std::runtime_error("configure() called before open_device()");
    }
    rtlsdr_set_sample_rate(dev_, cfg_.sample_rate);
    rtlsdr_set_center_freq(dev_, cfg_.center_freq);
    rtlsdr_set_freq_correction(dev_, cfg_.ppm);

    if (cfg_.agc || cfg_.gain_tenths_db == 0) {
        rtlsdr_set_tuner_gain_mode(dev_, 0);
    } else {
        rtlsdr_set_tuner_gain_mode(dev_, 1);
        rtlsdr_set_tuner_gain(dev_, cfg_.gain_tenths_db);
    }

    // Bias tee powers an inline LNA / antenna amplifier.
    rtlsdr_set_bias_tee(dev_, cfg_.bias_tee ? 1 : 0);

    rtlsdr_reset_buffer(dev_);
}

void RtlSource::read(std::vector<uint8_t>& buffer, size_t count) {
    if (!dev_) {
        throw std::runtime_error("read() called before open_device()");
    }
    buffer.resize(count * 2);
    int n_read = 0;
    if (rtlsdr_read_sync(dev_, buffer.data(), static_cast<int>(buffer.size()),
                         &n_read) != 0) {
        throw std::runtime_error("rtlsdr_read_sync failed");
    }
    buffer.resize(static_cast<size_t>(n_read));
}

}  // namespace openitc
