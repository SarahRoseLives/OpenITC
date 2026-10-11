#include "dsp.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace openitc {

std::vector<cf> to_baseband(const std::vector<uint8_t>& iq8) {
    const size_t n = iq8.size() / 2;
    std::vector<cf> out(n);
    for (size_t k = 0; k < n; ++k) {
        out[k] = cf(static_cast<float>(iq8[2 * k]) - 127.5f,
                    static_cast<float>(iq8[2 * k + 1]) - 127.5f);
    }
    return out;
}

void mix_shift(std::vector<cf>& x, double freq_hz, double sample_rate) {
    const double w = -2.0 * M_PI * freq_hz / sample_rate;
    const cf step(std::cos(w), std::sin(w));
    cf phase(1.0f, 0.0f);
    for (auto& s : x) {
        s *= phase;
        phase *= step;
        // renormalise occasionally to avoid drift
        if (std::abs(phase) < 0.5f || std::abs(phase) > 2.0f) {
            phase /= std::abs(phase);
        }
    }
}

FirDecimator::FirDecimator(int factor, double cutoff_norm, int ntaps)
    : factor_(factor < 1 ? 1 : factor) {
    if (ntaps < 3) ntaps = 3;
    if (ntaps % 2 == 0) ++ntaps;  // odd => symmetric
    const int M = (ntaps - 1) / 2;
    taps_.resize(ntaps);
    double sum = 0.0;
    for (int i = 0; i < ntaps; ++i) {
        const double t = i - M;
        const double sinc = (t == 0) ? 2.0 * cutoff_norm
                                     : std::sin(2.0 * M_PI * cutoff_norm * t) /
                                           (M_PI * t);
        const double hann =
            0.5 - 0.5 * std::cos(2.0 * M_PI * i / (ntaps - 1));
        taps_[i] = static_cast<float>(sinc * hann);
        sum += taps_[i];
    }
    for (auto& t : taps_) t = static_cast<float>(t / sum);
}

void FirDecimator::process(const std::vector<cf>& in,
                           std::vector<cf>& out) const {
    const int L = static_cast<int>(taps_.size());
    const int M = (L - 1) / 2;
    out.clear();
    if (static_cast<int>(in.size()) < L) return;
    out.reserve(in.size() / factor_ + 1);
    for (int base = 0; base + L <= static_cast<int>(in.size());
         base += factor_) {
        cf acc(0.0f, 0.0f);
        for (int i = 0; i < L; ++i) {
            acc += in[base + i] * taps_[i];
        }
        out.push_back(acc);
    }
    (void)M;
}

}  // namespace openitc
