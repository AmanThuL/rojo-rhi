//----------------------------------------------------------------------------------------------------------------------
/// @file ScopeRecord.h
/// @brief Declares one scope as a command list records it and the hook through which a backend fills its duration.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace rojoRHI::core {
    /// The `ScopeRecord::timestamp` of a scope no backend timed.
    inline constexpr uint32_t kNoTimestamp = UINT32_MAX;

    /// One scope as a command list recorded it: the label, a view into the frame arena or a static default, the depth, and
    /// the backend's timestamp pair.
    struct ScopeRecord
    {
        std::string_view label; ///< The scope's label, valid until the frame slot of its list is rewound.
        uint32_t depth = 0;     ///< Nesting depth; zero for an outermost scope.
        /// The index of the scope's begin timestamp in the backend's storage for the frame slot; the end timestamp is the
        /// next index. A backend that times scopes sets it on the record right after the core entry that recorded the scope;
        /// `kNoTimestamp` when none was written.
        uint32_t timestamp = kNoTimestamp;
    };

    /// Fills the durations of one queue's retired scopes: `records` are the scopes recorded for the queue in frame slot
    /// `slot`, and `report[i]` is the entry made for `records[i]`, with a duration of zero. Runs at the frame wait under
    /// the scope log's lock, so it must not call the core; both spans are valid only during the call. `context` is the
    /// backend's.
    using ScopeTimesFn = void (*)(void* context, uint32_t slot, std::span<const ScopeRecord> records, std::span<ScopeTiming> report);
} // namespace rojoRHI::core
