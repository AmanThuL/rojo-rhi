//----------------------------------------------------------------------------------------------------------------------
/// @file CommitTiming.h
/// @brief Declares the Metal 4 backend's private, test-only seam that reports each commit's GPU time.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstdint>

// This seam is not part of RojoRHI's interface: no public header declares it, and its only user is the benchmark
// harness, which measures the GPU time of RojoRHI's commits with the same commit feedback the native arm attaches.
namespace rojoRHI::metal4 {
    struct CommitTime
    {
        QueueType queue;
        uint64_t serial; // Per queue, counted from 1 from the first commit after setCommitTiming.
        double gpuStartSeconds;
        double gpuEndSeconds;
        bool failed;
    };

    using CommitTimingCallback = void (*)(void* user, const CommitTime& time);

    // Every later commit of `queue` reports through `callback` on Metal's completion thread; a null callback detaches.
    // Commits made before detaching may still report afterwards, so the caller keeps `user` alive until that work has
    // completed. The harness relies on exactly one commit per `Queue::submit` and no backend-internal commit while
    // timing is attached; the feedback coexists with the stager's own commit handler. Calls on MTL4::CommitOptions are
    // not counted in Device::counters.
    void setCommitTiming(Queue queue, CommitTimingCallback callback, void* user);
} // namespace rojoRHI::metal4
