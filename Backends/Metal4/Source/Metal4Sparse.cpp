//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Sparse.cpp
/// @brief Implements the Metal 4 placement sparse helpers: format support, measured tile shapes and the native tile mappings.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Sparse.h"

#include <rojoRHI/RHI.h>

#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Core/Sparse.h"
#include "Metal4Common.h"
#include "Metal4Device.h"

#include <Metal/Metal.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <vector>

namespace rojoRHI::metal4 {
    namespace {
        //==============================================================================================================
        NS::UInteger ceilDiv(uint64_t value, uint64_t unit)
        {
            return static_cast<NS::UInteger>((value + unit - 1) / unit);
        }

        //==============================================================================================================
        /// Returns the group of `groups` that updates the resource of `target` from its heap in the mode of `mapping`, adding it
        /// at the end when no element named that resource, heap and mode before.
        MappingGroup& groupFor(std::vector<MappingGroup>& groups, const TileMapping& mapping, const MappingTarget& target)
        {
            for (MappingGroup& group : groups)
            {
                if (group.texture == target.texture && group.buffer == target.buffer && group.heap == target.heap && group.unmap == mapping.unmap)
                {
                    return group;
                }
            }
            MappingGroup& group = groups.emplace_back();
            group.texture = target.texture;
            group.buffer = target.buffer;
            group.heap = target.heap;
            group.unmap = mapping.unmap;
            return group;
        }

        //==============================================================================================================
        /// Adds the operations of `mapping`, a texture region or mip tail of `target`, to `group`: one per layer.
        void addTextureOperations(MappingGroup& group, const TileMapping& mapping, const MappingTarget& target)
        {
            const TileShape& shape = target.shape;
            const TextureRegion& region = mapping.region;
            const MTL::SparseTextureMappingMode mode = mapping.unmap ? MTL::SparseTextureMappingModeUnmap : MTL::SparseTextureMappingModeMap;
            MTL4::UpdateSparseTextureMappingOperation operation = {};
            operation.mode = mode;
            NS::UInteger layerTiles = 0;
            if (mapping.mipTail)
            {
                // Metal maps a tail as a row of its tiles at the first tail level.
                layerTiles = ceilDiv(shape.mipTailBytes, core::kTileBytes);
                operation.textureLevel = shape.mipTailFirst;
                operation.textureRegion = MTL::Region(0, 0, 0, layerTiles, 1, 1);
            }
            else
            {
                const NS::UInteger width = ceilDiv(region.extent.width, shape.width);
                const NS::UInteger height = ceilDiv(region.extent.height, shape.height);
                const NS::UInteger depth = ceilDiv(region.extent.depth, shape.depth);
                layerTiles = width * height * depth;
                operation.textureLevel = region.mip;
                operation.textureRegion =
                    MTL::Region(region.offset.x / shape.width, region.offset.y / shape.height, region.offset.z / shape.depth, width, height, depth);
            }
            // An unmap's heap offset is zero (VU-rojo-sparse-0010), and Metal reads none.
            const NS::UInteger firstTile = mapping.unmap ? 0 : static_cast<NS::UInteger>(mapping.heapOffset / core::kTileBytes);
            for (uint32_t layer = 0; layer < region.layers; ++layer)
            {
                operation.textureSlice = region.layer + layer;
                operation.heapOffset = mapping.unmap ? 0 : firstTile + layer * layerTiles;
                group.textureOperations.push_back(operation);
            }
        }

        //==============================================================================================================
        /// Adds the operation of `mapping`, a buffer range, to `group`.
        void addBufferOperation(MappingGroup& group, const TileMapping& mapping)
        {
            const MTL::SparseTextureMappingMode mode = mapping.unmap ? MTL::SparseTextureMappingModeUnmap : MTL::SparseTextureMappingModeMap;
            const NS::Range range(static_cast<NS::UInteger>(mapping.bufferOffset / core::kTileBytes), ceilDiv(mapping.bufferSize, core::kTileBytes));
            group.bufferOperations.push_back(
                MTL4::UpdateSparseBufferMappingOperation{ mode, range, static_cast<NS::UInteger>(mapping.heapOffset / core::kTileBytes) }
            );
        }

        //==============================================================================================================
        /// Signals the progress event of `queue` with its next value and waits on the queue for that value: the queue's later
        /// work, a native mapping update included, starts only once all its earlier work, a mapping update included, completes.
        void orderOnQueue(QueueImpl& queue, core::DeviceCore& core)
        {
            const uint64_t progress = queue.progressValue.load(std::memory_order_relaxed) + 1;
            queue.progressValue.store(progress, std::memory_order_relaxed);
            queue.native->signalEvent(queue.progress, progress);
            queue.native->wait(queue.progress, progress);
            core::DeviceCounts::add(core.counts().nativeSubmitCalls, 2);
        }
    } // namespace

    //==================================================================================================================
    MTL::Size sparseTileExtent(MTL::Device* device, MTL::TextureType type, MTL::PixelFormat format)
    {
        return device->sparseTileSize(type, format, 1, MTL::SparsePageSize64);
    }

    //==================================================================================================================
    bool sparseFormat(MTL::Device* device, MTL::PixelFormat format)
    {
        return device->supportsPlacementSparse() && sparseTileExtent(device, MTL::TextureType2D, format).width != 0;
    }

    //==================================================================================================================
    TileShape measureTiles(MTL::Device* device, const MTL::Texture* texture, uint32_t mips)
    {
        const MTL::Size extent = device->sparseTileSize(texture->textureType(), texture->pixelFormat(), texture->sampleCount(), MTL::SparsePageSize64);
        TileShape shape;
        shape.width = static_cast<uint32_t>(extent.width);
        shape.height = static_cast<uint32_t>(extent.height);
        shape.depth = static_cast<uint32_t>(extent.depth);
        shape.bytes = static_cast<uint32_t>(device->sparseTileSizeInBytes(MTL::SparsePageSize64));
        // Metal reports a first tail level past the mip chain, with a nonzero tail size, for a texture whose levels all tile.
        shape.mipTailFirst = static_cast<uint32_t>(std::min<NS::UInteger>(texture->firstMipmapInTail(), mips));
        shape.mipTailBytes = shape.mipTailFirst < mips ? texture->tailSizeInBytes() : 0;
        return shape;
    }

    //==================================================================================================================
    std::vector<MappingTarget> resolveMappings(core::DeviceCore& core, std::span<const TileMapping> mappings)
    {
        std::vector<MappingTarget> targets(mappings.size());
        for (size_t index = 0; index < mappings.size(); ++index)
        {
            const TileMapping& mapping = mappings[index];
            MappingTarget& target = targets[index];
            // The core has checked every handle in Debug; in Release an element naming no live resource is misuse that no tier
            // detects, and it is skipped as the core skips its state.
            if (mapping.texture.bits != 0)
            {
                const core::LookupResult<core::TextureRecord> found = core.pool<Texture>().lookup(mapping.texture);
                if (found.status != core::LookupStatus::Ok)
                {
                    continue;
                }
                target.texture = static_cast<const MTL::Texture*>(found.record->native);
                target.shape = core::textureTileShape(core, *found.record);
            }
            else
            {
                const core::LookupResult<core::BufferRecord> found = core.pool<Buffer>().lookup(mapping.buffer);
                if (found.status != core::LookupStatus::Ok)
                {
                    continue;
                }
                target.buffer = static_cast<const MTL::Buffer*>(found.record->native);
                target.shape = core::bufferTileShape();
            }
            target.heap = static_cast<const MTL::Heap*>(core.pool<Heap>().native(mapping.heap));
        }
        return targets;
    }

    //==================================================================================================================
    std::vector<MappingGroup> groupMappings(std::span<const TileMapping> mappings, std::span<const MappingTarget> targets)
    {
        std::vector<MappingGroup> groups;
        std::vector<MappingGroup> bufferGroups;
        for (size_t index = 0; index < mappings.size(); ++index)
        {
            const TileMapping& mapping = mappings[index];
            const MappingTarget& target = targets[index];
            if (target.texture == nullptr && target.buffer == nullptr)
            {
                continue;
            }
            if (target.texture != nullptr)
            {
                addTextureOperations(groupFor(groups, mapping, target), mapping, target);
            }
            else
            {
                addBufferOperation(groupFor(bufferGroups, mapping, target), mapping);
            }
        }
        groups.insert(groups.end(), std::make_move_iterator(bufferGroups.begin()), std::make_move_iterator(bufferGroups.end()));
        return groups;
    }

    //==================================================================================================================
    void mapTiles(QueueImpl& queue, std::span<const TileMapping> mappings)
    {
        core::DeviceCore& core = queue.device->core;
        core::mapTiles(core, queue.type, mappings);
        if (mappings.empty())
        {
            return;
        }
        const std::vector<MappingTarget> targets = resolveMappings(core, mappings);
        const std::vector<MappingGroup> groups = groupMappings(mappings, targets);
        if (groups.empty())
        {
            return;
        }
        const AutoreleasePool pool;
        // A native mapping update is not ordered against the command buffers around it on its queue: it may apply while work
        // submitted before it still reads the old tiles, and work submitted after it may run before it applies. The queue's
        // own event orders both sides. The signal and wait after the updates also keep a texture update that follows a buffer
        // update on the queue from faulting in Metal's driver, which any queue operation between the two avoids.
        orderOnQueue(queue, core);
        for (const MappingGroup& group : groups)
        {
            if (group.texture != nullptr)
            {
                queue.native->updateTextureMappings(group.texture, group.heap, group.textureOperations.data(), group.textureOperations.size());
            }
            else
            {
                queue.native->updateBufferMappings(group.buffer, group.heap, group.bufferOperations.data(), group.bufferOperations.size());
            }
            core::DeviceCounts::add(core.counts().nativeSubmitCalls);
        }
        orderOnQueue(queue, core);
    }
} // namespace rojoRHI::metal4
