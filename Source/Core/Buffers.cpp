//----------------------------------------------------------------------------------------------------------------------
/// @file Buffers.cpp
/// @brief Implements the checks of the buffer chapter.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Buffers.h"

#include "Base/Assert.h"
#include "Core/Heaps.h"
#include "Core/Label.h"
#include "Core/Sparse.h"
#include "Core/Tracker/Tracker.h"

#include <string>
#include <utility>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        [[maybe_unused]] bool hasUsage(BufferUsage usage, BufferUsage flag)
        {
            return (usage & flag) != BufferUsage::None;
        }
    } // namespace

    //==================================================================================================================
    void checkBufferDesc(const DeviceCore& core, [[maybe_unused]] const BufferDesc& desc)
    {
        [[maybe_unused]] const Capabilities& capabilities = core.capabilities();
        ROJORHI_VU(
            desc.memory == Memory::Gpu || !hasUsage(desc.usage, BufferUsage::ShaderWrite | BufferUsage::Sparse),
            "VU-rojo-buffer-0003",
            "ShaderWrite or Sparse is set on a buffer whose memory is not Gpu"
        );
        ROJORHI_VU(
            desc.memory != Memory::Readback || !hasUsage(desc.usage, BufferUsage::Index | BufferUsage::IndirectArgs),
            "VU-rojo-buffer-0004",
            "Index or IndirectArgs is set on a Readback buffer"
        );
        ROJORHI_VU(
            !hasUsage(desc.usage, BufferUsage::Sparse) || capabilities.placementSparse,
            "VU-rojo-buffer-0005",
            "Sparse is set but the device reports no sparse resources"
        );
        ROJORHI_VU(
            desc.size > 0 && desc.size <= capabilities.limits.maxBufferSize,
            "VU-rojo-buffer-0006",
            "size {} is not in 1 to {}",
            desc.size,
            capabilities.limits.maxBufferSize
        );
    }

    //==================================================================================================================
    BufferRecord prepareBuffer(
        DeviceCore& core, const BufferDesc& desc, std::span<const std::byte> initial, const Placement& placement, const PlacementRequirements& requirements
    )
    {
        checkBufferDesc(core, desc);
        ROJORHI_VU(initial.size() <= desc.size, "VU-rojo-buffer-0007", "{} initial bytes exceed the buffer size {}", initial.size(), desc.size);
        ROJORHI_VU(
            initial.empty() || (!hasUsage(desc.usage, BufferUsage::Sparse) && desc.memory != Memory::Readback),
            "VU-rojo-buffer-0008",
            "initial data is given for a Sparse or Readback buffer"
        );
        if (placement.heap.bits != 0)
        {
            checkPlacement(
                checkedHeap(core, placement.heap), placement, requirements, PlacedResource{ desc.memory, false, hasUsage(desc.usage, BufferUsage::Sparse) }
            );
        }
        BufferRecord record;
        record.heap = placement.heap;
        record.heapOffset = placement.heap.bits != 0 ? placement.offset : 0;
        record.label = resolveLabel(desc.label, "rojorhi.buffer.unnamed");
        record.size = desc.size;
        record.initialBytes = initial.size();
        record.memory = desc.memory;
        record.usage = desc.usage;
        return record;
    }

    //==================================================================================================================
    Buffer addBuffer(DeviceCore& core, BufferRecord record)
    {
        if (record.heap.bits != 0)
        {
            addPlacedResource(core, record.heap);
        }
#ifndef NDEBUG
        // The tracker copies what it follows before the record moves into the pool.
        const std::string label = record.label;
        const uint64_t size = record.size;
        const Heap heap = record.heap;
        const uint64_t heapOffset = record.heapOffset;
        const bool sparse = (record.usage & BufferUsage::Sparse) != BufferUsage::None;
        const Buffer buffer = core.pool<Buffer>().allocate(std::move(record));
        trackBuffer(core, buffer, label, size, heap, heapOffset, sparse);
        return buffer;
#else
        return core.pool<Buffer>().allocate(std::move(record));
#endif
    }

    //==================================================================================================================
    Buffer addFrameArenaPage(DeviceCore& core, BufferRecord record)
    {
        return core.pool<Buffer>().allocate(std::move(record));
    }

    //==================================================================================================================
    void destroyBuffer(DeviceCore& core, Buffer buffer)
    {
        // A bad handle is left to the destroy below, which reports VU-rojo-core-0001 to 0003 and releases nothing.
        Heap heap = {};
        TileMap tiles;
        core.pool<Buffer>().update(buffer, [&](BufferRecord& record) {
            heap = record.heap;
            tiles = std::move(record.mapped);
        });
        core.destroy(buffer);
        forgetBuffer(core, buffer);
        // Destroying a sparse buffer unmaps its tiles at the call, so its heaps may be destroyed before the slot retires.
        releaseTiles(core, tiles);
        if (heap.bits != 0)
        {
            removePlacedResource(core, heap);
        }
    }

    //==================================================================================================================
    BufferRecord& checkedBuffer(DeviceCore& core, Buffer buffer)
    {
        BufferRecord* record = core.pool<Buffer>().checked(buffer, false);
        ROJORHI_INTERNAL_ASSERT(record != nullptr, "a checked lookup that does not allow null returned null");
        return *record;
    }

    //==================================================================================================================
    void checkAddrOffset([[maybe_unused]] uint64_t offset, [[maybe_unused]] uint64_t maxAddrOffset)
    {
        ROJORHI_VU(offset < maxAddrOffset, "VU-rojo-buffer-0011", "offset {} reaches maxAddrOffset {}", offset, maxAddrOffset);
    }

    //==================================================================================================================
    void checkBufferAddress([[maybe_unused]] const BufferRecord& record, [[maybe_unused]] uint64_t offset, [[maybe_unused]] uint64_t maxAddrOffset)
    {
        ROJORHI_VU(offset < record.size, "VU-rojo-buffer-0012", "offset {} is not inside the buffer \"{}\" of {} bytes", offset, record.label, record.size);
        ROJORHI_VU(record.memory != Memory::Readback, "VU-rojo-buffer-0013", "the buffer \"{}\" is a Readback buffer and has no address", record.label);
        checkAddrOffset(offset, maxAddrOffset);
    }

    //==================================================================================================================
    void checkCpuAccess([[maybe_unused]] const BufferRecord& record)
    {
        ROJORHI_VU(record.memory != Memory::Gpu, "VU-rojo-buffer-0014", "the buffer \"{}\" is a Gpu buffer and is not mapped", record.label);
    }

    //==================================================================================================================
    void checkCopySource([[maybe_unused]] const BufferRecord& record)
    {
        ROJORHI_VU(
            record.memory == Memory::Gpu || record.memory == Memory::Upload,
            "VU-rojo-buffer-0002",
            "the buffer \"{}\" is the source of a copy but its memory is neither Gpu nor Upload",
            record.label
        );
    }

    //==================================================================================================================
    void checkCopyDestination([[maybe_unused]] const BufferRecord& record)
    {
        ROJORHI_VU(
            record.memory == Memory::Gpu || record.memory == Memory::Readback,
            "VU-rojo-buffer-0001",
            "the buffer \"{}\" is the destination of a copy but its memory is neither Gpu nor Readback",
            record.label
        );
    }
} // namespace rojoRHI::core
