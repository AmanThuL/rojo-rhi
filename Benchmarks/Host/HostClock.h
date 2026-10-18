//----------------------------------------------------------------------------------------------------------------------
/// @file HostClock.h
/// @brief Declares the host clock every CPU span is read from: mach_absolute_time ticks and their nanosecond scale.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI::bench {
    uint64_t hostTicks();
    // Converts a tick count, or a difference of two, with the timebase read on the first call.
    double ticksToNanoseconds(uint64_t ticks);
} // namespace rojoRHI::bench
