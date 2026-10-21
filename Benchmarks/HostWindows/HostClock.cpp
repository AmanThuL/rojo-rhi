//----------------------------------------------------------------------------------------------------------------------
/// @file HostClock.cpp
/// @brief Implements the host clock over QueryPerformanceCounter and QueryPerformanceFrequency.
//----------------------------------------------------------------------------------------------------------------------
#include "Host/HostClock.h"

#include <windows.h>

namespace rojoRHI::bench {
    //==================================================================================================================
    uint64_t hostTicks()
    {
        LARGE_INTEGER ticks{};
        QueryPerformanceCounter(&ticks);
        return static_cast<uint64_t>(ticks.QuadPart);
    }

    //==================================================================================================================
    // The frequency is fixed at boot, so it is read once; QueryPerformanceFrequency cannot fail on Windows XP or later.
    double ticksToNanoseconds(uint64_t ticks)
    {
        static const double nanosecondsPerTick = [] {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            return 1.0e9 / static_cast<double>(frequency.QuadPart);
        }();
        return static_cast<double>(ticks) * nanosecondsPerTick;
    }
} // namespace rojoRHI::bench
