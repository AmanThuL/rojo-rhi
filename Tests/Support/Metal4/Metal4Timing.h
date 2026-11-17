//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Timing.h
/// @brief Declares what Metal 4 count pins call: the guard before counts that include timestamp writes, and the encoder labels.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Counters.h"

#include <cstdint>

namespace rojoRHI::test {
    // Fails the case when a scope the list recorded (its scopes, passes and upscales) holds no timestamp pair. A count pin
    // that includes timestamp writes calls it before reading the counts and before `Queue::submit`, which takes the scopes.
    // A pair is missing without `DeviceDesc::scopeTimings`, when Metal refused a counter heap, or past the slot's capacity.
    void requireTimed(CommandList list);

    const core::RecordCounts& timedCounts(CommandList list);

    // 1 when the list's encoders carry a label, one `setLabel` each (validation, capture or the validation layer), else 0,
    // so a count pin holds with and without the layer.
    uint64_t encoderLabel(CommandList list);

    uint64_t encoderLabel(Device device);

    // 1 when the device was created with `DeviceDesc::debugMarkers` or `DeviceDesc::enableCapture` (ADR 0034): scopes,
    // passes and upscales then push and pop a debug group, each a native call.
    uint64_t debugGroup(CommandList list);
} // namespace rojoRHI::test
