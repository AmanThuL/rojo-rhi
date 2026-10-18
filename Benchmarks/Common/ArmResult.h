//----------------------------------------------------------------------------------------------------------------------
/// @file ArmResult.h
/// @brief Declares what one run of one arm measures: per-frame values, RojoRHI's counters, outputs and the Null trace.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/Config.h"

#include <rojoRHI/RHI.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    // A texture or buffer a workload writes, read back after the measured frames.
    struct Output
    {
        std::string name;
        std::vector<std::byte> bytes;
    };

    // The native calls and barriers an arm counted over a window narrower than its whole frame() call.
    struct CallWindow
    {
        uint64_t calls = 0;
        uint64_t barriers = 0;
    };

    // What an arm's frame() measures itself: the CPU span of metric 5 and the GPU span of its commits. Either is empty
    // where it does not exist (the composed frame has no CPU span; Null has no GPU time). `counted` is empty when the
    // frame's native calls are those of the whole frame() call; C9 counts its submit calls only, the window its CPU span
    // times, because its lists are recorded before that span. RojoRHI folds a list's recorded calls into its counters at
    // submission, so R's window takes only the queue and host calls of that window.
    struct FrameResult
    {
        std::optional<double> cpuNs;
        std::optional<double> gpuNs;
        std::optional<CallWindow> counted;
    };

    // The native calls an arm has made so far: RojoRHI's from Device::counters, N's from its own increments.
    struct CallSnapshot
    {
        uint64_t calls = 0;               ///< RojoRHI: nativeRecordCalls + nativeSubmitCalls + nativeHostCalls.
        uint64_t barriers = 0;            ///< RojoRHI: nativeBarriers, which calls already includes.
        std::optional<Counters> counters; ///< RojoRHI only.
    };

    struct ArmResult
    {
        Arm arm = Arm::Rojo;
        bool ok = true;
        std::string error; ///< Names the configuration and what failed; empty when ok.

        // One entry per measured frame.
        std::vector<std::optional<double>> cpuNs;
        std::vector<std::optional<double>> gpuNs;
        std::vector<uint64_t> allocations; ///< Malloc-zone allocations of every thread during the whole frame() call.
        std::vector<uint64_t> nativeCalls;
        std::vector<uint64_t> nativeBarriers;
        std::optional<std::vector<uint64_t>> cpuAllocations; ///< RojoRHI's own Counters::cpuAllocations; R only.

        // R only: the device's counters after warm-up and after the measured frames, and the first measured frame's
        // delta of every field.
        std::optional<Counters> countersAfterWarmup;
        std::optional<Counters> countersEnd;
        std::optional<Counters> frameCounters;

        std::vector<Output> outputs;
        std::vector<std::string_view> trace; ///< Null builds with RunOptions::trace: R's calls in the first measured frame.
    };
} // namespace rojoRHI::bench
