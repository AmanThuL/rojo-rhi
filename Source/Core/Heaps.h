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
    struct PlacedResource
    {
        Memory memory = Memory::Gpu; // Always Gpu for a texture.
        bool texture = false;
        bool sparse = false;
    };

    HeapRecord prepareHeap(const HeapDesc& desc);

    HeapRecord& checkedHeap(DeviceCore& core, Heap heap);

    void checkBufferRequirementsQuery(const DeviceCore& core, const BufferDesc& desc);

    // The one site of the placement rules, shared by buffers and textures. `placement.heap` is not null.
    void checkPlacement(const HeapRecord& heap, const Placement& placement, const PlacementRequirements& requirements, const PlacedResource& resource);

    void addPlacedResource(DeviceCore& core, Heap heap);

    void removePlacedResource(DeviceCore& core, Heap heap);

    void destroyHeap(DeviceCore& core, Heap heap);
} // namespace rojoRHI::core
