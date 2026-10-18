//----------------------------------------------------------------------------------------------------------------------
/// @file RojoCalls.h
/// @brief Declares the list and queue types the RojoRHI arm records through: plain on Metal 4, tracing on Null.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Rojo/RojoTrace.h"

#include <rojoRHI/RHI.h>

#include <string_view>

namespace rojoRHI::bench {
#if defined(ROJORHI_BENCH_BACKEND_NULL)
    // Null builds trace every recorded call, so one workload source yields both its counts and its call sequence.
    using List = TracedList;
    using QueueRef = TracedQueue;
#else
    // Metal 4 builds, which are timed, record through RojoRHI's own types with nothing in between.
    using List = CommandList;
    using QueueRef = Queue;
#endif

    // Device::acquireCommandList, logged as "acquireCommandList" on Null builds.
    inline List acquire(Device device, QueueType type, std::string_view label)
    {
#if defined(ROJORHI_BENCH_BACKEND_NULL)
        traceCall("acquireCommandList");
#endif
        return List(device.acquireCommandList(type, label));
    }
} // namespace rojoRHI::bench
