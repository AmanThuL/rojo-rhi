//----------------------------------------------------------------------------------------------------------------------
/// @file Scopes.h
/// @brief Declares how a command list records the scopes that `Device::scopeTimings` reports.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/CommandLists.h"

#include <cstdint>
#include <string_view>

namespace rojoRHI::core {
    /// Records one scope on `list` at nesting depth `depth`, in the order scopes begin so a parent precedes its children. A
    /// non-empty `label` is copied into the list's frame-arena page, which takes no lock but a page claim; an empty one is
    /// replaced by `defaultLabel`, which must outlive the frame. It checks nothing.
    void recordScope(CommandListCore& list, std::string_view label, std::string_view defaultLabel, uint32_t depth);
} // namespace rojoRHI::core
