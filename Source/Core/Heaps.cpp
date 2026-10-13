//----------------------------------------------------------------------------------------------------------------------
/// @file Heaps.cpp
/// @brief Implements the checks of the heap and placement rules.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Heaps.h"

#include "Base/Assert.h"
#include "Core/Buffers.h"
#include "Core/Label.h"

namespace rojoRHI::core {
    //==================================================================================================================
    HeapRecord prepareHeap(const HeapDesc& desc)
    {
        ROJORHI_VU(desc.size > 0, "VU-rojo-heap-0001", "a heap has size zero");
        HeapRecord record;
        record.label = resolveLabel(desc.label, "rojorhi.heap.unnamed");
        record.size = desc.size;
        record.memory = desc.memory;
        return record;
    }

    //==================================================================================================================
    HeapRecord& checkedHeap(DeviceCore& core, Heap heap)
    {
        HeapRecord* record = core.pool<Heap>().checked(heap, false);
        ROJORHI_INTERNAL_ASSERT(record != nullptr, "a checked lookup that does not allow null returned null");
        return *record;
    }

    //==================================================================================================================
    void checkBufferRequirementsQuery(const DeviceCore& core, const BufferDesc& desc)
    {
        checkBufferDesc(core, desc);
        ROJORHI_VU((desc.usage & BufferUsage::Sparse) == BufferUsage::None, "VU-rojo-heap-0002", "placement requirements are requested for a Sparse buffer");
    }

    //==================================================================================================================
    void checkPlacement(
        [[maybe_unused]] const HeapRecord& heap,
        [[maybe_unused]] const Placement& placement,
        [[maybe_unused]] const PlacementRequirements& requirements,
        [[maybe_unused]] const PlacedResource& resource
    )
    {
        ROJORHI_INTERNAL_ASSERT(
            requirements.alignment != 0 && (requirements.alignment & (requirements.alignment - 1)) == 0, "the alignment of a placement is not a power of two"
        );
        ROJORHI_VU(
            placement.offset <= heap.size && requirements.size <= heap.size - placement.offset,
            "VU-rojo-heap-0003",
            "{} bytes at offset {} do not fit the heap \"{}\" of {} bytes",
            requirements.size,
            placement.offset,
            heap.label,
            heap.size
        );
        ROJORHI_VU(
            placement.offset % requirements.alignment == 0,
            "VU-rojo-heap-0004",
            "offset {} is not a multiple of the alignment {}",
            placement.offset,
            requirements.alignment
        );
        ROJORHI_VU(
            resource.memory == heap.memory && (!resource.texture || heap.memory == Memory::Gpu),
            "VU-rojo-heap-0005",
            "a {} is placed in the heap \"{}\" of another memory class",
            resource.texture ? "texture" : "buffer",
            heap.label
        );
        ROJORHI_VU(!resource.sparse, "VU-rojo-heap-0006", "a Sparse resource is placed in the heap \"{}\"", heap.label);
    }

    //==================================================================================================================
    void addPlacedResource(DeviceCore& core, Heap heap)
    {
        [[maybe_unused]] const LookupStatus status = core.pool<Heap>().update(heap, [](HeapRecord& record) { ++record.placedResources; });
        ROJORHI_INTERNAL_ASSERT(status == LookupStatus::Ok, "a resource is placed in a heap that is gone");
    }

    //==================================================================================================================
    void removePlacedResource(DeviceCore& core, Heap heap)
    {
        // The heap may already be gone when its resource is destroyed after the heap was misused; that is VU-rojo-device-0008's.
        core.pool<Heap>().update(heap, [](HeapRecord& record) {
            ROJORHI_INTERNAL_ASSERT(record.placedResources > 0, "a heap counts fewer placed resources than it holds");
            --record.placedResources;
        });
    }

    //==================================================================================================================
    void destroyHeap(DeviceCore& core, Heap heap)
    {
        // A null, stale or foreign handle reaches the destroy below, which reports VU-rojo-core-0001 to 0003.
        [[maybe_unused]] uint64_t placed = 0;
        [[maybe_unused]] uint64_t mapped = 0;
        [[maybe_unused]] std::string label;
        [[maybe_unused]] const LookupStatus status = core.pool<Heap>().update(heap, [&](HeapRecord& record) {
            placed = record.placedResources;
            mapped = record.mappedTiles;
            label = record.label;
        });
        ROJORHI_VU(
            status != LookupStatus::Ok || placed == 0, "VU-rojo-device-0008", "the heap \"{}\" is destroyed with {} placed resources alive", label, placed
        );
        ROJORHI_VU(
            status != LookupStatus::Ok || mapped == 0, "VU-rojo-device-0009", "the heap \"{}\" is destroyed with {} tiles still mapped from it", label, mapped
        );
        core.destroy(heap);
    }
} // namespace rojoRHI::core
