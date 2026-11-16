//----------------------------------------------------------------------------------------------------------------------
/// @file Heaps.h
/// @brief Declares the checks of the heap and placement rules, shared by every backend and by buffers and textures.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Records.h"

#include <cstdint>

namespace rojoRHI::core {
    /// What the placement rules need to know about the resource being created.
    struct PlacedResource
    {
        Memory memory = Memory::Gpu; ///< The resource's memory class; a texture is always Gpu.
        bool texture = false;        ///< True for a texture.
        bool sparse = false;         ///< True when the usage contains Sparse.
    };

    /// Checks a heap description (VU-rojo-heap-0001, VU-rojo-core-0006, VU-rojo-core-0007) and returns the record of the heap it
    /// describes with no native payload. A backend calls it first in `createHeap`.
    HeapRecord prepareHeap(const HeapDesc& desc);

    /// Finds the live record `heap` names (VU-rojo-core-0001 to 0003).
    HeapRecord& checkedHeap(DeviceCore& core, Heap heap);

    /// Checks the arguments of `Device::placementRequirements` for a buffer (VU-rojo-heap-0002): the descriptor obeys the buffer
    /// rules and is not Sparse.
    void checkBufferRequirementsQuery(const DeviceCore& core, const BufferDesc& desc);

    /// Checks that `resource`, whose `requirements` the backend computed, fits `placement` inside `heap` (VU-rojo-heap-0003 to
    /// 0006); the one site of each rule, shared by buffers and textures. `placement.heap` is not null.
    void checkPlacement(const HeapRecord& heap, const Placement& placement, const PlacementRequirements& requirements, const PlacedResource& resource);

    /// Notes that one more live resource is placed in `heap`, which `checkedHeap` found.
    void addPlacedResource(DeviceCore& core, Heap heap);

    /// Notes that a resource placed in `heap` was destroyed.
    void removePlacedResource(DeviceCore& core, Heap heap);

    /// Destroys `heap`, deferred as `DeviceCore::destroy` does, after checking that no resource placed in it is alive
    /// (VU-rojo-device-0008).
    void destroyHeap(DeviceCore& core, Heap heap);
} // namespace rojoRHI::core
