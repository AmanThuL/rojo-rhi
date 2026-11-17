//----------------------------------------------------------------------------------------------------------------------
/// @file Scopes.h
/// @brief Declares how a command list records the scopes that `Device::scopeTimings` reports.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/CommandLists.h"

#include <cstdint>
#include <string_view>

namespace rojoRHI::core {
    // With scope timings, a non-empty `label` is copied into the list's frame-arena page (a page claim, the one lock) unless it
    // equals the list's last copy this frame; without them the caller's text is kept, read only inside the recording entry.
    // `defaultLabel` must outlive the frame. Checks nothing.
    void recordScope(CommandListCore& list, std::string_view label, std::string_view defaultLabel, uint32_t depth);
} // namespace rojoRHI::core
