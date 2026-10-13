//----------------------------------------------------------------------------------------------------------------------
/// @file QueueType.h
/// @brief Declares the kinds of queue a device exposes.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The queue a command list is recorded for and submitted to. `Graphics` accepts every command, `Compute` accepts compute,
    /// copy, fill, clear and scope commands, and `Copy` accepts copy and scope commands.
    enum class QueueType : uint8_t
    {
        Graphics, ///< The graphics queue.
        Compute,  ///< The compute queue.
        Copy      ///< The copy queue.
    };
} // namespace rojoRHI
