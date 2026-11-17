//----------------------------------------------------------------------------------------------------------------------
/// @file Accesses.h
/// @brief Declares the access declarations of chapter 14 that a command list keeps for the validation tracker.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"

#include <span>

namespace rojoRHI::core {
    // Does nothing on a device without validation; Release checks nothing and takes no lock.
    void declareAccess(CommandListCore& list, std::span<const ResourceAccess> accesses);

    void consumeAccessesChecked(CommandListCore& list, bool dispatch, bool work);

    void checkNoPendingAccessesChecked(const CommandListCore& list);

    // Each declaration becomes a tracker use of the command unless `work` is false (a zero count); the pending set empties
    // either way. Inline, so Release compiles the call away.
    inline void consumeAccesses([[maybe_unused]] CommandListCore& list, [[maybe_unused]] bool dispatch, [[maybe_unused]] bool work)
    {
#ifndef NDEBUG
        consumeAccessesChecked(list, dispatch, work);
#endif
    }

    inline void checkNoPendingAccesses([[maybe_unused]] const CommandListCore& list)
    {
#ifndef NDEBUG
        checkNoPendingAccessesChecked(list);
#endif
    }
} // namespace rojoRHI::core
