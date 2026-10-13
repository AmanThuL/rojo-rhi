//----------------------------------------------------------------------------------------------------------------------
/// @file Placement.h
/// @brief Declares where inside a heap a resource is created.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Handles.h>

#include <cstdint>

namespace rojoRHI {
    /// Passed to `Device::createBuffer` and `Device::createTexture` to create the resource inside a heap. With a null `heap`
    /// the resource is a dedicated allocation. Otherwise, with `req` the `PlacementRequirements` of the descriptor, `offset`
    /// plus `req.size` is at most the heap's size (VU-rojo-heap-0003), `offset` is a multiple of `req.alignment`
    /// (VU-rojo-heap-0004), the resource's memory class equals the heap's and a texture goes only in a `Memory::Gpu` heap
    /// (VU-rojo-heap-0005), and the resource is not `Sparse` (VU-rojo-heap-0006). The heap outlives the resource
    /// (VU-rojo-device-0008).
    struct Placement
    {
        /// The heap to place the resource in; null selects a dedicated allocation.
        Heap heap = {};
        /// Byte offset inside `heap`.
        uint64_t offset = 0;
    };
} // namespace rojoRHI
