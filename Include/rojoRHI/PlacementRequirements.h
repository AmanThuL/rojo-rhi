//----------------------------------------------------------------------------------------------------------------------
/// @file PlacementRequirements.h
/// @brief Declares what a resource needs from a heap.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The answer of `Device::placementRequirements`: the bytes a resource occupies in a heap and the alignment its offset
    /// must satisfy. The same descriptor gives the same value on one device.
    struct PlacementRequirements
    {
        /// Bytes the resource occupies in a heap.
        uint64_t size = 0;
        /// Power of two the heap offset is a multiple of.
        uint64_t alignment = 0;
    };
} // namespace rojoRHI
