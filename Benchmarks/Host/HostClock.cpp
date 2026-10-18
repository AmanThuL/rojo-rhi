//----------------------------------------------------------------------------------------------------------------------
/// @file HostClock.cpp
/// @brief Implements the host clock over mach_absolute_time and mach_timebase_info.
//----------------------------------------------------------------------------------------------------------------------
#include "Host/HostClock.h"

#include <mach/mach_time.h>

namespace rojoRHI::bench {
    //==================================================================================================================
    uint64_t hostTicks()
    {
        return mach_absolute_time();
    }

    //==================================================================================================================
    double ticksToNanoseconds(uint64_t ticks)
    {
        static const mach_timebase_info_data_t timebase = [] {
            mach_timebase_info_data_t info{};
            mach_timebase_info(&info);
            return info;
        }();
        return static_cast<double>(ticks) * timebase.numer / timebase.denom;
    }
} // namespace rojoRHI::bench
