#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace openitc {

using cf = std::complex<float>;

// Convert interleaved uint8 I/Q (RTL-SDR native) to complex float, DC-centred.
std::vector<cf> to_baseband(const std::vector<uint8_t>& iq8);

// Shift the complex signal by `freq_hz` (in place).
void mix_shift(std::vector<cf>& x, double freq_hz, double sample_rate);

// Integer-factor FIR low-pass decimator (windowed-sinc taps).
class FirDecimator {
public:
    FirDecimator(int factor, double cutoff_norm, int ntaps);

    // `out` receives every `factor`-th filtered sample.
    void process(const std::vector<cf>& in, std::vector<cf>& out) const;

    int factor() const { return factor_; }

private:
    int factor_;
    std::vector<float> taps_;
};

}  // namespace openitc
