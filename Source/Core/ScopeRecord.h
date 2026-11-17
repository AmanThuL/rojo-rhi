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
    inline constexpr uint32_t kNoTimestamp = UINT32_MAX;

    // The label views the frame arena or a static default. On a device without scope timings it may be the caller's text,
    // valid only inside the recording entry; such a record is never submitted.
    struct ScopeRecord
    {
        std::string_view label;
        uint32_t depth = 0;
        // The begin timestamp's index in the backend's storage for the frame slot; the end is the next index. A timing backend
        // sets it right after the core entry that recorded the scope.
        uint32_t timestamp = kNoTimestamp;
    };

    // Runs at the frame wait under the scope log's lock, so it must not call the core; both spans are valid only during the
    // call. `report[i]` is the entry of `records[i]`, with a duration of zero, and the queue is `report[0].queue`.
    using ScopeTimesFn = void (*)(void* context, uint32_t slot, std::span<const ScopeRecord> records, std::span<ScopeTiming> report);
} // namespace rojoRHI::core
