//----------------------------------------------------------------------------------------------------------------------
/// @file SubmitTiming.h
/// @brief Declares the D3D12 backend's private, test-only seam that reports each submission's GPU time.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstdint>

// This seam is not part of RojoRHI's interface: no public header declares it, and its only user is the benchmark
// harness, which measures the GPU time of RojoRHI's submissions with the same two timestamps the native arm writes.
namespace rojoRHI::d3d12 {
    struct SubmitTime
    {
        QueueType queue;
        uint64_t serial; // Per queue, counted from 1 from the first submission with lists after setSubmitTiming.
        double gpuStartSeconds;
        double gpuEndSeconds;
        bool failed;
    };

    using SubmitTimingCallback = void (*)(void* user, const SubmitTime& time);

    // Attaches `callback` to `queue`; a null callback detaches. While attached, each Queue::submit with lists makes one
    // report, from its first list's first timestamp to its last list's last, in seconds of QueryPerformanceCounter, so
    // every queue shares one time base; only lists acquired after this call are timed. A submission without lists makes
    // no report and takes no serial. Reports are made only inside drainSubmitTiming, on its thread; submitting makes no
    // native call for the seam. Detaching never blocks and discards unreported submissions; each attachment's memory
    // stays until destroyDevice. A report is `failed`, with zero times, when its first or last list was acquired
    // outside this attachment, the queue writes no timestamps (Copy without copy-queue timestamps), more submissions
    // were made or lists acquired between two drains than the seam holds, or the drain's signal failed, its wait timed
    // out or the device was removed; nothing aborts. No native call of the seam counts in Device::counters. The caller
    // serializes submit, setSubmitTiming and drainSubmitTiming on one queue; lists may be acquired on any thread
    // meanwhile.
    void setSubmitTiming(Queue queue, SubmitTimingCallback callback, void* user);

    // Signals the seam's fence on `queue` once and waits for it, so every submission that is to report and has not has
    // completed, then makes each report on the calling thread, in serial order; returns at once, making no native call,
    // while the queue is detached or has nothing to report.
    void drainSubmitTiming(Queue queue);
} // namespace rojoRHI::d3d12
