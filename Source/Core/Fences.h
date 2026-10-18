//----------------------------------------------------------------------------------------------------------------------
/// @file Fences.h
/// @brief Declares the fence record operations and the queue and fence checks of chapter 10.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Records.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace rojoRHI::core {
    /// Checks a fence's label (VU-rojo-core-0006, VU-rojo-core-0007) and returns the record of a fence created at `initial` with
    /// no native payload. A backend calls it first in `createFence`.
    FenceRecord prepareFence(uint64_t initial, std::string_view label);

    /// Checks the fences of a submission before a backend runs it (VU-rojo-queue-0003, VU-rojo-queue-0007, VU-rojo-core-0001 to
    /// 0003 for each fence) and counts one `nativeSubmitCalls` for each wait and signal; the backend counts each native
    /// submission it commits. A backend calls it in `Queue::submit` after `submitLists`.
    void checkSubmit(DeviceCore& core, std::span<const FenceValue> wait, std::span<const FenceValue> signal);

    /// Whether a backend's work completes within the submission call or later, through `completeFence`.
    enum class SignalCompletion
    {
        Immediate, ///< Every signal is reached before the submission returns.
        Later      ///< The backend reports each signal through `completeFence`.
    };

    /// Notes that each value of `signal`, which `checkSubmit` accepted, was submitted for signal; later submissions are checked
    /// against it. With `Immediate` the completed value is raised in the same locked update, so no reader sees a value
    /// submitted but not completed.
    void recordSignals(DeviceCore& core, std::span<const FenceValue> signal, SignalCompletion completion);

    /// Raises the completed value of `value.fence` to `value.value` when it is greater; a backend calls it as the work that
    /// signals the fence completes.
    void completeFence(DeviceCore& core, FenceValue value);

    /// Returns the greatest value `fence` has reached (VU-rojo-core-0001 to 0003); the validation tracker then finds the work
    /// that value orders complete.
    uint64_t completedValue(DeviceCore& core, Fence fence);

    /// Checks the arguments of `Device::wait` (VU-rojo-queue-0008, VU-rojo-core-0001 to 0003) and returns true when
    /// `value.fence` has already reached `value.value`; the validation tracker then finds the work it orders complete.
    bool checkWait(DeviceCore& core, FenceValue value, uint64_t timeoutNs);

    /// Destroys `fence`, deferred like any resource, and forgets the clocks the validation tracker kept of its values. A
    /// backend calls it from `Device::destroy(Fence)`.
    void destroyFence(DeviceCore& core, Fence fence);
} // namespace rojoRHI::core
