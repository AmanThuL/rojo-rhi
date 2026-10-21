//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Timing.h
/// @brief Declares what Metal 4 count pins call: the guard before counts that include timestamp writes, and the encoder labels.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Counters.h"

#include <cstdint>

namespace rojoRHI::test {
    /// Fails the running case, naming the reason, when a scope `list` has recorded so far (its own scopes, its render
    /// passes and its upscales) holds no timestamp pair. A count pin that includes timestamp writes calls it before it reads
    /// the counts, and before `Queue::submit`, which takes the list's scopes: without a pair the count is two calls short,
    /// because the device was created without `DeviceDesc::scopeTimings`, Metal refused a counter heap (its limit holds per
    /// process or on the system) or the slot's capacity was passed.
    void requireTimed(CommandList list);

    /// Returns the counts `list` has recorded, after `requireTimed(list)`.
    const core::RecordCounts& timedCounts(CommandList list);

    /// Returns 1 when the encoders `list` opens carry a label, one `setLabel` call each (on a device created with validation
    /// or capture, or under the validation layer), else 0, so a count pin holds with and without the layer.
    uint64_t encoderLabel(CommandList list);

    /// Returns 1 when the encoders the lists of `device` open carry a label, else 0, as `encoderLabel(CommandList)`.
    uint64_t encoderLabel(Device device);
} // namespace rojoRHI::test
