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
    FenceRecord prepareFence(uint64_t initial, std::string_view label);

    // Counts one `nativeSubmitCalls` per wait and signal. Called after `submitLists`. Release reads no record, so it takes
    // no lock.
    void checkSubmit(DeviceCore& core, std::span<const FenceValue> wait, std::span<const FenceValue> signal);

    enum class SignalCompletion
    {
        Immediate, // Every signal is reached before the submission returns.
        Later      // The backend reports each signal through `completeFence`.
    };

    void recordSignals(DeviceCore& core, std::span<const FenceValue> signal, SignalCompletion completion);

    void completeFence(DeviceCore& core, FenceValue value);

    // Tells the validation tracker the work that value orders is complete.
    uint64_t completedValue(DeviceCore& core, Fence fence);

    // A reached value tells the tracker the work it orders is complete.
    bool checkWait(DeviceCore& core, FenceValue value, uint64_t timeoutNs);

    void destroyFence(DeviceCore& core, Fence fence);
} // namespace rojoRHI::core
