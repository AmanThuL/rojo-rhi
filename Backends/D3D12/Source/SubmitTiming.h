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
        uint64_t serial; ///< Per queue, counted from 1 from the first submission with lists after setSubmitTiming.
        double gpuStartSeconds;
        double gpuEndSeconds;
        bool failed;
    };

    using SubmitTimingCallback = void (*)(void* user, const SubmitTime& time);

    // Attaches `callback` to `queue`; a null callback detaches. While attached, every list of the queue's type acquired
    // afterwards writes a timestamp first and another last, after its last command and before its own resolves, and
    // resolves both into a buffer the seam allocated at attach; every Queue::submit that names lists then makes exactly
    // one report, whose start is its first list's first timestamp and whose end its last list's last, in seconds of the
    // CPU's QueryPerformanceCounter, through the calibration pair GetClockCalibration and GetTimestampFrequency gave at
    // attach, so the queues share one time base. A submission without lists makes no report and takes no serial.
    // Submitting makes no native call for the seam: reports are made only inside drainSubmitTiming, on its calling
    // thread, which signals a fence of the seam's own once on the queue, after every submission made so far, and waits
    // for it. Detaching never blocks: it discards the submissions not yet reported, and what the seam allocated stays
    // until destroyDevice, since a list acquired while it was attached may still write into it. A report is `failed`,
    // with zero times, when its first or last list was acquired while the queue was detached or attached by an earlier
    // call, when the queue writes no timestamps (a Copy queue on a device without copy-queue timestamps), when more
    // submissions or lists were made between two drains than the seam holds, or when the drain's signal was refused,
    // its wait timed out or the device was removed: a removed device fails the reports and aborts nothing. None of the
    // seam's native calls counts in Device::counters. Calls on one queue, Queue::submit, setSubmitTiming and
    // drainSubmitTiming, are serialized by the caller, as submissions on one queue already are; list acquisition may run
    // on any thread meanwhile.
    void setSubmitTiming(Queue queue, SubmitTimingCallback callback, void* user);

    // Signals the seam's fence on `queue` once and waits for it, so every submission that is to report and has not has
    // completed, then makes each report on the calling thread, in serial order; returns at once, making no native call,
    // while the queue is detached or has nothing to report.
    void drainSubmitTiming(Queue queue);
} // namespace rojoRHI::d3d12
