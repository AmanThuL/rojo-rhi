//----------------------------------------------------------------------------------------------------------------------
/// @file Runner.h
/// @brief Declares runArm, the one driver of every arm's warm-up and measured frames and of every measurement it takes.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/CounterFields.h"
#include "Common/RunOptions.h"
#include "Host/AllocationCounter.h"

#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI::bench {
    // A workload arm. Its constructor creates every object the workload uses through the context and records any
    // failure for ready(); frame(index) records, submits and waits one frame, index counting warm-up and measured
    // frames from 0; readback() returns the outputs after the measured frames, outside every measurement. The context,
    // NativeContext or RojoContext, owns the device and everything created through it.
    template<class A>
    concept WorkloadArm = requires(A& arm, typename A::Context& context, const RunOptions& options, uint32_t index) {
        { A::kArm } -> std::convertible_to<Arm>;
        requires std::constructible_from<A, typename A::Context&, const Config&>;
        { arm.ready() } -> std::convertible_to<const std::expected<void, std::string>&>;
        { arm.frame(index) } -> std::same_as<std::expected<FrameResult, std::string>>;
        { arm.readback() } -> std::same_as<std::expected<std::vector<Output>, std::string>>;
        { context.open(options) } -> std::same_as<std::expected<void, std::string>>;
        { context.callSnapshot() } -> std::same_as<CallSnapshot>;
    };

    //==================================================================================================================
    // Measures one arm. Per measured frame: the arm's CPU and GPU spans, the allocations of the whole frame() call, and
    // the native calls and barriers the frame made, read from the context after the allocation window closes. R adds
    // its counters after warm-up and at the end, each frame's cpuAllocations and the first measured frame's delta.
    template<WorkloadArm A> ArmResult runArm(const Config& config, const RunOptions& options)
    {
        ArmResult result;
        result.arm = A::kArm;
        const auto fail = [&](std::string_view stage, std::string_view error) {
            result.ok = false;
            result.error = std::format("{} arm {}: {}: {}", config.id, armName(A::kArm), stage, error);
            return std::move(result);
        };

        const uint32_t measured = options.measuredFrames;
        result.cpuNs.reserve(measured);
        result.gpuNs.reserve(measured);
        result.allocations.reserve(measured);
        result.nativeCalls.reserve(measured);
        result.nativeBarriers.reserve(measured);
        std::vector<uint64_t> cpuAllocations;
        cpuAllocations.reserve(measured);

        typename A::Context context;
        if (const std::expected<void, std::string> opened = context.open(options); !opened)
        {
            return fail("setup", opened.error());
        }
        A arm(context, config);
        if (const std::expected<void, std::string>& ready = arm.ready(); !ready)
        {
            return fail("setup", ready.error());
        }

        const uint32_t frames = options.warmupFrames + measured;
        for (uint32_t index = 0; index < frames; ++index)
        {
            const bool traced = options.trace && index == options.warmupFrames;
            if constexpr (requires { context.traceFrame(true); })
            {
                context.traceFrame(traced);
            }
            const CallSnapshot before = context.callSnapshot();
            if (index == options.warmupFrames)
            {
                result.countersAfterWarmup = before.counters;
            }
            const uint64_t allocationsBefore = allocationCount();
            const std::expected<FrameResult, std::string> frame = arm.frame(index);
            const uint64_t allocationsAfter = allocationCount();
            const CallSnapshot after = context.callSnapshot();
            if constexpr (requires { context.traceFrame(false); })
            {
                if (traced)
                {
                    context.traceFrame(false);
                    result.trace.assign(context.tracedCalls().begin(), context.tracedCalls().end());
                }
            }
            if (!frame)
            {
                return fail(std::format("frame {}", index), frame.error());
            }
            if (index < options.warmupFrames)
            {
                continue;
            }
            result.cpuNs.push_back(frame->cpuNs);
            result.gpuNs.push_back(frame->gpuNs);
            result.allocations.push_back(allocationsAfter - allocationsBefore);
            result.nativeCalls.push_back(after.calls - before.calls);
            result.nativeBarriers.push_back(after.barriers - before.barriers);
            if (after.counters && before.counters)
            {
                cpuAllocations.push_back(after.counters->cpuAllocations - before.counters->cpuAllocations);
                if (!result.frameCounters)
                {
                    result.frameCounters = counterDelta(*after.counters, *before.counters);
                }
            }
        }
        const CallSnapshot end = context.callSnapshot();
        if (end.counters)
        {
            result.countersEnd = end.counters;
            result.cpuAllocations = std::move(cpuAllocations);
        }

        std::expected<std::vector<Output>, std::string> outputs = arm.readback();
        if (!outputs)
        {
            return fail("readback", outputs.error());
        }
        result.outputs = std::move(*outputs);
        return result;
    }
} // namespace rojoRHI::bench
