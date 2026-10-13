//----------------------------------------------------------------------------------------------------------------------
/// @file FenceValue.h
/// @brief Declares a point on a fence's timeline.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Handles.h>

#include <cstdint>

namespace rojoRHI {
    /// A point on a fence's timeline, used as a wait or a signal by `Queue::submit` and as a target by `Device::wait`.
    struct FenceValue
    {
        Fence fence;    ///< A fence created by `Device::createFence`.
        uint64_t value; ///< The timeline value.
    };
} // namespace rojoRHI
