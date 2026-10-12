#pragma once

#include "model.h"

namespace openitc {

// Populates the decoded sub-struct on `e` (beacon/time/cntl/wiu) according to
// e.type and e.payload. Leaves fields untouched for unrecognized types.
void parse_event(PacketEvent& e);

}  // namespace openitc
