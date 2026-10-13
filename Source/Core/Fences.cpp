//----------------------------------------------------------------------------------------------------------------------
/// @file Fences.cpp
/// @brief Implements the fence record operations and the checks of the queue and fence rules.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Fences.h"

#include "Base/Assert.h"
#include "Core/Label.h"
#include "Core/Tracker/Tracker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        /// Finds the live record `fence` names (VU-rojo-core-0001 to 0003) and returns a copy taken under the pool's lock, so a
        /// free-threaded reader never sees a record a submission is changing.
        FenceRecord snapshot(DeviceCore& core, Fence fence)
        {
            [[maybe_unused]] FenceRecord* found = core.pool<Fence>().checked(fence, false);
            ROJORHI_INTERNAL_ASSERT(found != nullptr, "a checked lookup that does not allow null returned null");
            FenceRecord copy;
            core.pool<Fence>().update(fence, [&](FenceRecord& record) { copy = record; });
            return copy;
        }

        //==============================================================================================================
        /// A value a fence can reach without further submissions: at most its initial value, or one already submitted for signal.
        /// Submitted values only grow, so every value up to the greatest one is reachable.
        [[maybe_unused]] bool reachable(const FenceRecord& record, uint64_t value)
        {
            return value <= record.submitted;
        }
    } // namespace

    //==================================================================================================================
    FenceRecord prepareFence(uint64_t initial, std::string_view label)
    {
        FenceRecord record;
        record.label = resolveLabel(label, "rojorhi.fence.unnamed");
        record.initial = initial;
        record.submitted = initial;
        record.completed = initial;
        return record;
    }

    //==================================================================================================================
    void checkSubmit(DeviceCore& core, std::span<const FenceValue> wait, std::span<const FenceValue> signal)
    {
        // One native submission and one native call for each wait and signal it carries.
        DeviceCounts::add(core.counts().nativeSubmitCalls, 1 + wait.size() + signal.size());
        for (const FenceValue& entry : wait)
        {
            [[maybe_unused]] const FenceRecord record = snapshot(core, entry.fence);
            ROJORHI_VU(
                reachable(record, entry.value),
                "VU-rojo-queue-0007",
                "the wait on fence \"{}\" names value {}, above its initial value {} and every value submitted for signal ({})",
                record.label,
                entry.value,
                record.initial,
                record.submitted
            );
        }
        for (size_t i = 0; i < signal.size(); ++i)
        {
            [[maybe_unused]] const FenceRecord record = snapshot(core, signal[i].fence);
            ROJORHI_VU(
                std::none_of(
                    signal.begin(), signal.begin() + static_cast<std::ptrdiff_t>(i), [&](const FenceValue& earlier) { return earlier.fence == signal[i].fence; }
                ),
                "VU-rojo-queue-0003",
                "fence \"{}\" appears more than once in the signal list",
                record.label
            );
            ROJORHI_VU(
                signal[i].value > record.initial,
                "VU-rojo-queue-0003",
                "the signal of fence \"{}\" is value {}, not above its initial value {}",
                record.label,
                signal[i].value,
                record.initial
            );
            ROJORHI_VU(
                signal[i].value > record.submitted,
                "VU-rojo-queue-0003",
                "the signal of fence \"{}\" is value {}, not above the greatest value {} already submitted for signal on it",
                record.label,
                signal[i].value,
                record.submitted
            );
        }
    }

    //==================================================================================================================
    void recordSignals(DeviceCore& core, std::span<const FenceValue> signal, SignalCompletion completion)
    {
        for (const FenceValue& entry : signal)
        {
            // One update raises both values, so a concurrent wait never sees a value submitted but not yet completed.
            core.pool<Fence>().update(entry.fence, [&](FenceRecord& record) {
                record.submitted = std::max(record.submitted, entry.value);
                if (completion == SignalCompletion::Immediate)
                {
                    record.completed = std::max(record.completed, entry.value);
                }
            });
        }
    }

    //==================================================================================================================
    void completeFence(DeviceCore& core, FenceValue value)
    {
        core.pool<Fence>().update(value.fence, [&](FenceRecord& record) { record.completed = std::max(record.completed, value.value); });
    }

    //==================================================================================================================
    uint64_t completedValue(DeviceCore& core, Fence fence)
    {
        DeviceCounts::add(core.counts().nativeHostCalls);
        const uint64_t completed = snapshot(core, fence).completed;
        // The pool's lock is released: the tracker's lock is never taken under it.
        observeFence(core, fence, completed);
        return completed;
    }

    //==================================================================================================================
    bool checkWait(DeviceCore& core, FenceValue value, [[maybe_unused]] uint64_t timeoutNs)
    {
        const FenceRecord record = snapshot(core, value.fence);
        ROJORHI_VU(
            timeoutNs != UINT64_MAX || reachable(record, value.value),
            "VU-rojo-queue-0008",
            "an unlimited wait on fence \"{}\" names value {}, above its initial value {} and every value submitted for signal ({})",
            record.label,
            value.value,
            record.initial,
            record.submitted
        );
        const bool reached = record.completed >= value.value;
        if (reached)
        {
            // The pool's lock is released: the tracker's lock is never taken under it.
            observeFence(core, value.fence, value.value);
        }
        return reached;
    }

    //==================================================================================================================
    void destroyFence(DeviceCore& core, Fence fence)
    {
        core.destroy(fence);
        forgetFence(core, fence);
    }
} // namespace rojoRHI::core
