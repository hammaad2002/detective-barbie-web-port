// In-game motion at the display rate: see smooth.cpp.
#pragma once

#include <cstdint>

#include "recomp.h"

namespace smooth {

/** Called by timeGetTime with the real clock; returns the time the calling guest code should see. */
uint32_t time_get_time(Cpu* c, uint32_t now);

}  // namespace smooth
