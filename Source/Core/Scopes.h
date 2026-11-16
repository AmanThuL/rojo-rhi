//----------------------------------------------------------------------------------------------------------------------
/// @file Scopes.h
/// @brief Declares how a command list records the scopes that `Device::scopeTimings` reports.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/CommandLists.h"

#include <cstdint>
#include <string_view>

namespace rojoRHI::core {
    /// Records one scope on `list` at nesting depth `depth`, in the order scopes begin so a parent precedes its children. On
    /// a device with scope timings, a non-empty `label` is copied into the list's frame-arena page, which takes no lock but a
    /// page claim, unless it equals the list's last copy of this frame, which the record then shares; without them the record
    /// keeps the caller's `label`, which no one reads once the recording entry returns. An empty one is replaced by
    /// `defaultLabel`, which must outlive the frame. It checks nothing.
    void recordScope(CommandListCore& list, std::string_view label, std::string_view defaultLabel, uint32_t depth);
} // namespace rojoRHI::core
