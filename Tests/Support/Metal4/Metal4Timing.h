//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Timing.h
/// @brief Declares the guard that Metal 4 count pins call before reading counts that include timestamp writes.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Counters.h"

namespace rojoRHI::test {
    /// Fails the running case, naming the reason, when a scope `list` has recorded so far (its own scopes, its render
    /// passes and its upscales) holds no timestamp pair. A count pin that includes timestamp writes calls it before it reads
    /// the counts, and before `Queue::submit`, which takes the list's scopes: without a pair the count is two calls short,
    /// because Metal refused a counter heap (its limit holds per process or on the system) or the slot's capacity was passed.
    void requireTimed(CommandList list);

    /// Returns the counts `list` has recorded, after `requireTimed(list)`.
    const core::RecordCounts& timedCounts(CommandList list);
} // namespace rojoRHI::test
