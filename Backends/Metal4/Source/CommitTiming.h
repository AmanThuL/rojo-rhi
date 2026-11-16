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
        uint64_t serial; ///< Per queue, counted from 1 from the first commit after setCommitTiming.
        double gpuStartSeconds;
        double gpuEndSeconds;
        bool failed;
    };

    using CommitTimingCallback = void (*)(void* user, const CommitTime& time);

    // Every later commit of `queue` carries MTL4::CommitOptions feedback that calls `callback` from Metal's completion
    // thread; a null callback detaches. Detaching does not cancel the reports of commits already made, which hold copies of
    // `callback` and `user`: they may still arrive afterwards, so the caller keeps `user` alive until the queue's earlier work has
    // completed. A submission with no lists makes no commit and no report. Calls on MTL4::CommitOptions are not counted in Device::counters.
    // The harness relies on one contract: exactly one commit per Queue::submit, and no backend-internal commit on the
    // queue while timing is attached to it, because RojoGpuTiming fails a frame on a stray or non-contiguous serial
    // (RojoContext's beginFrame(commits) names the count it expects). The feedback must also coexist with a commit
    // feedback handler the backend sets for its own waits, as the stager does.
    void setCommitTiming(Queue queue, CommitTimingCallback callback, void* user);
} // namespace rojoRHI::metal4
