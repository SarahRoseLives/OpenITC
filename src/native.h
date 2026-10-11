#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "dsp.h"
#include "rtl_source.h"

namespace openitc {

// Capture `channel` and channelize to the mode's baseband rate (native).
std::vector<cf> capture_channel(long channel, const std::string& mode,
                                const RtlConfig& cfg, size_t nsamples);

// `openitc demod ...` : native demod + sync detection.
int run_demod(int argc, char** argv);

// `openitc monitor ...` : wideband scan of all PTC channels, listing packets
// as they arrive until interrupted.
int run_monitor(int argc, char** argv);

}  // namespace openitc
