//----------------------------------------------------------------------------------------------------------------------
/// @file Accesses.h
/// @brief Declares the access declarations of chapter 14 that a command list keeps for the validation tracker.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/ResourceAccess.h>

#include "Core/CommandLists.h"

#include <span>

namespace rojoRHI::core {
    /// Declares `accesses` on `list` (VU-rojo-diag-0004, then for each entry VU-rojo-diag-0001, 0002, 0003, 0005, and
    /// VU-rojo-core-0001 to 0003 for its resource) and appends them to the list's pending set. Only a device created with
    /// validation checks or keeps anything: without it the call returns at once. Release builds check nothing and take no lock.
    void declareAccess(CommandListCore& list, std::span<const ResourceAccess> accesses);

    /// Does the work of `consumeAccesses` in a Debug build.
    void consumeAccessesChecked(CommandListCore& list, bool dispatch, bool work);

    /// Does the work of `checkNoPendingAccesses` in a Debug build.
    void checkNoPendingAccessesChecked(const CommandListCore& list);

    /// Consumes the pending set at the entry of a draw or, when `dispatch` is set, of a dispatch (VU-rojo-diag-0006): each
    /// declared stage is one the command runs. Each declaration becomes a use of the command for the validation tracker,
    /// unless `work` is false: a command with a zero count makes no use, though it still consumes the set. The set is empty
    /// afterwards. Inline, so a Release build compiles the call to nothing.
    inline void consumeAccesses([[maybe_unused]] CommandListCore& list, [[maybe_unused]] bool dispatch, [[maybe_unused]] bool work)
    {
#ifndef NDEBUG
        consumeAccessesChecked(list, dispatch, work);
#endif
    }

    /// Checks that no declaration is pending when `list` ends (VU-rojo-diag-0007). Inline, as `consumeAccesses` is.
    inline void checkNoPendingAccesses([[maybe_unused]] const CommandListCore& list)
    {
#ifndef NDEBUG
        checkNoPendingAccessesChecked(list);
#endif
    }
} // namespace rojoRHI::core
