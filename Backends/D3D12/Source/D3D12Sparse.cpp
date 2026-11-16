//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Sparse.cpp
/// @brief Implements D3D12 reserved resources, their tile shapes, the tile shape entries and tile mapping on any queue.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Sparse.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Core/Sparse.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Device.h"
#include "D3D12Textures.h"

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI {
    namespace {
        static_assert(core::kTileBytes == static_cast<uint32_t>(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES), "a sparse tile is one D3D12 tile");

        /// The resource one group of mapping elements updates, the heap it maps from, null for an unmap, and the regions of
        /// its elements in element order: one `UpdateTileMappings` call.
        struct MappingGroup
        {
            ID3D12Resource* resource = nullptr;     ///< The reserved resource.
            ID3D12Heap* heap = nullptr;             ///< The heap the tiles map from; null when they are unmapped.
            std::vector<d3d12::TileRegion> regions; ///< Every region of the group's elements.
        };

        //==============================================================================================================
        /// Returns the tiles `texels` texels span when a tile covers `tile` of them, a partial last tile included.
        UINT tilesAlong(uint64_t texels, uint64_t tile)
        {
            return static_cast<UINT>((texels + tile - 1) / tile);
        }

        //==============================================================================================================
        /// Returns the reserved form of `native`: the same resource with `layout`, as `CreateReservedResource2` takes it.
        D3D12_RESOURCE_DESC reservedDesc(const D3D12_RESOURCE_DESC1& native, D3D12_TEXTURE_LAYOUT layout)
        {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = native.Dimension;
            desc.Alignment = 0;
            desc.Width = native.Width;
            desc.Height = native.Height;
            desc.DepthOrArraySize = native.DepthOrArraySize;
            desc.MipLevels = native.MipLevels;
            desc.Format = native.Format;
            desc.SampleDesc = native.SampleDesc;
            desc.Layout = layout;
            desc.Flags = native.Flags;
            return desc;
        }

        //==============================================================================================================
        /// Returns the `Unsupported` error of the Sparse texture `label`, which D3D12 cannot reserve because of `why`.
        std::unexpected<Error> refused(std::string_view label, std::string_view why)
        {
            return std::unexpected(Error{ ErrorCategory::Unsupported, std::format("the Sparse texture \"{}\" is not supported: {}", label, why) });
        }

        //==============================================================================================================
        /// Returns the group of `groups` that updates `resource` from `heap`, adding it at the end when no earlier element
        /// named that resource and heap.
        MappingGroup& groupFor(std::vector<MappingGroup>& groups, ID3D12Resource* resource, ID3D12Heap* heap)
        {
            for (MappingGroup& group : groups)
            {
                if (group.resource == resource && group.heap == heap)
                {
                    return group;
                }
            }
            MappingGroup& group = groups.emplace_back();
            group.resource = resource;
            group.heap = heap;
            return group;
        }

        //==============================================================================================================
        /// Returns the groups of `mappings`, one per resource and heap in the order their first element appears. The core has
        /// checked every handle in Debug; in Release an element naming no live resource, or a map whose heap slot holds no
        /// native heap, is misuse that no tier detects, and it is skipped.
        std::vector<MappingGroup> groupMappings(core::DeviceCore& core, std::span<const TileMapping> mappings)
        {
            std::vector<MappingGroup> groups;
            for (const TileMapping& mapping : mappings)
            {
                auto* heap = mapping.unmap ? nullptr : static_cast<ID3D12Heap*>(core.pool<Heap>().native(mapping.heap));
                if (!mapping.unmap && heap == nullptr)
                {
                    continue;
                }
                if (mapping.texture.bits != 0)
                {
                    const core::LookupResult<core::TextureRecord> found = core.pool<Texture>().lookup(mapping.texture);
                    if (found.status != core::LookupStatus::Ok)
                    {
                        continue;
                    }
                    ID3D12Resource* resource = static_cast<const d3d12::TexturePayload*>(found.record->native)->resource;
                    const std::vector<d3d12::TileRegion> regions = d3d12::tileCoordinates(*found.record, mapping);
                    std::vector<d3d12::TileRegion>& target = groupFor(groups, resource, heap).regions;
                    target.insert(target.end(), regions.begin(), regions.end());
                    continue;
                }
                const core::LookupResult<core::BufferRecord> found = core.pool<Buffer>().lookup(mapping.buffer);
                if (found.status != core::LookupStatus::Ok)
                {
                    continue;
                }
                ID3D12Resource* resource = static_cast<const d3d12::BufferPayload*>(found.record->native)->resource;
                groupFor(groups, resource, heap).regions.push_back(d3d12::tileCoordinates(mapping));
            }
            return groups;
        }

        //==============================================================================================================
        /// Issues the one `UpdateTileMappings` call of `group` on `queue`: each region maps a range of heap tiles from its
        /// start, or to null for an unmap. Counts one `nativeSubmitCalls`.
        void updateMappings(QueueImpl& queue, const MappingGroup& group)
        {
            const size_t count = group.regions.size();
            std::vector<D3D12_TILED_RESOURCE_COORDINATE> starts(count);
            std::vector<D3D12_TILE_REGION_SIZE> sizes(count);
            std::vector<D3D12_TILE_RANGE_FLAGS> flags(count, group.heap == nullptr ? D3D12_TILE_RANGE_FLAG_NULL : D3D12_TILE_RANGE_FLAG_NONE);
            std::vector<UINT> heapStarts(count);
            std::vector<UINT> tileCounts(count);
            for (size_t index = 0; index < count; ++index)
            {
                const d3d12::TileRegion& region = group.regions[index];
                starts[index] = region.start;
                sizes[index] = region.size;
                heapStarts[index] = region.heapStart;
                tileCounts[index] = region.size.NumTiles;
            }
            const auto regions = static_cast<UINT>(count);
            queue.native->UpdateTileMappings(
                group.resource,
                regions,
                starts.data(),
                sizes.data(),
                group.heap,
                regions,
                flags.data(),
                heapStarts.data(),
                tileCounts.data(),
                D3D12_TILE_MAPPING_FLAG_NONE
            );
            core::DeviceCounts::add(queue.device->core.counts().nativeSubmitCalls);
        }
    } // namespace

    namespace d3d12 {
        //==============================================================================================================
        Result<Com<ID3D12Resource>> createReservedBuffer(DeviceImpl& impl, const D3D12_RESOURCE_DESC1& desc, std::string_view label)
        {
            D3D12_RESOURCE_DESC reserved = reservedDesc(desc, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
            reserved.Width = (desc.Width + core::kTileBytes - 1) / core::kTileBytes * core::kTileBytes;
            Com<ID3D12Resource> resource;
            if (const HRESULT created = impl.device()->CreateReservedResource2(
                    &reserved, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(resource.put())
                );
                FAILED(created))
            {
                const std::string what = std::format("ID3D12Device10::CreateReservedResource2 of buffer \"{}\" ({} bytes)", label, reserved.Width);
                return std::unexpected(failure(impl, created, what));
            }
            return resource;
        }

        //==============================================================================================================
        Result<Com<ID3D12Resource>> createReservedTexture(
            DeviceImpl& impl, const TextureDesc& desc, const D3D12_RESOURCE_DESC1& native, std::span<const DXGI_FORMAT> castable, std::string_view label
        )
        {
            if (desc.type == TextureType::Tex1D || desc.type == TextureType::Tex1DArray)
            {
                return refused(label, "D3D12 reserves no one-dimensional texture");
            }
            if (desc.type == TextureType::Tex3D && impl.features.tiledResourcesTier < static_cast<uint32_t>(D3D12_TILED_RESOURCES_TIER_3))
            {
                return refused(
                    label, std::format("a 3D reserved texture needs tiled-resources tier 3 and the device reports {}", impl.features.tiledResourcesTier)
                );
            }
            if (!impl.core.formatCapabilities(desc.format).sparse)
            {
                return refused(label, std::format("the device does not tile format {}", static_cast<uint32_t>(desc.format)));
            }
            const D3D12_RESOURCE_DESC reserved = reservedDesc(native, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE);
            const auto castableCount = static_cast<UINT32>(castable.size());
            const DXGI_FORMAT* castableList = castableCount == 0 ? nullptr : castable.data();
            Com<ID3D12Resource> resource;
            if (const HRESULT created = impl.device()->CreateReservedResource2(
                    &reserved, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, castableCount, castableList, IID_PPV_ARGS(resource.put())
                );
                FAILED(created))
            {
                return std::unexpected(failure(impl, created, std::format("ID3D12Device10::CreateReservedResource2 for texture \"{}\"", label)));
            }
            return resource;
        }

        //==============================================================================================================
        TileShape readTileShape(ID3D12Device12& device, ID3D12Resource& resource, uint32_t mips)
        {
            D3D12_PACKED_MIP_INFO packed = {};
            D3D12_TILE_SHAPE extent = {};
            device.GetResourceTiling(&resource, nullptr, &packed, &extent, nullptr, 0, nullptr);
            const bool tail = packed.NumPackedMips != 0;
            TileShape shape;
            shape.width = extent.WidthInTexels;
            shape.height = extent.HeightInTexels;
            shape.depth = extent.DepthInTexels;
            shape.bytes = core::kTileBytes;
            shape.mipTailFirst = tail ? uint32_t{ packed.NumStandardMips } : mips;
            shape.mipTailBytes = tail ? uint64_t{ packed.NumTilesForPackedMips } * core::kTileBytes : 0;
            return shape;
        }

        //==============================================================================================================
        std::vector<TileRegion> tileCoordinates(const core::TextureRecord& texture, const TileMapping& mapping)
        {
            const TileShape& shape = texture.tiles;
            ROJORHI_INTERNAL_ASSERT(shape.bytes == core::kTileBytes, "a reserved texture has no tile shape measured at creation");
            const TextureRegion& region = mapping.region;
            TileRegion layerRegion;
            uint32_t mip = region.mip;
            if (mapping.mipTail)
            {
                // A packed tail has no standard shape: it is a run of tiles counted along x from the first packed mip.
                mip = shape.mipTailFirst;
                layerRegion.size.NumTiles = tilesAlong(shape.mipTailBytes, core::kTileBytes);
                layerRegion.size.UseBox = FALSE;
            }
            else
            {
                const UINT width = tilesAlong(region.extent.width, shape.width);
                const UINT height = tilesAlong(region.extent.height, shape.height);
                const UINT depth = tilesAlong(region.extent.depth, shape.depth);
                layerRegion.start.X = region.offset.x / shape.width;
                layerRegion.start.Y = region.offset.y / shape.height;
                layerRegion.start.Z = region.offset.z / shape.depth;
                layerRegion.size.NumTiles = width * height * depth;
                layerRegion.size.UseBox = TRUE;
                layerRegion.size.Width = width;
                // A level is at most 16,384 texels on an axis, so its tiles fit 16 bits.
                layerRegion.size.Height = static_cast<UINT16>(height);
                layerRegion.size.Depth = static_cast<UINT16>(depth);
            }
            // An unmap's heap offset is zero (VU-rojo-sparse-0010), and a null range reads none.
            const UINT firstTile = mapping.unmap ? 0 : static_cast<UINT>(mapping.heapOffset / core::kTileBytes);
            std::vector<TileRegion> regions(region.layers, layerRegion);
            for (uint32_t layer = 0; layer < region.layers; ++layer)
            {
                regions[layer].start.Subresource = mip + (region.layer + layer) * texture.mips;
                regions[layer].heapStart = mapping.unmap ? 0 : firstTile + layer * layerRegion.size.NumTiles;
            }
            return regions;
        }

        //==============================================================================================================
        TileRegion tileCoordinates(const TileMapping& mapping)
        {
            TileRegion region;
            region.start.X = static_cast<UINT>(mapping.bufferOffset / core::kTileBytes);
            region.size.NumTiles = tilesAlong(mapping.bufferSize, core::kTileBytes);
            region.size.UseBox = FALSE;
            region.heapStart = mapping.unmap ? 0 : static_cast<UINT>(mapping.heapOffset / core::kTileBytes);
            return region;
        }
    } // namespace d3d12

    //==================================================================================================================
    TileShape Device::tileShape(Texture texture)
    {
        return core::tileShape(DeviceImpl::of(*this).core, texture);
    }

    //==================================================================================================================
    TileShape Device::tileShape(Buffer buffer)
    {
        return core::tileShape(DeviceImpl::of(*this).core, buffer);
    }

    //==================================================================================================================
    void Queue::mapTiles(std::span<const TileMapping> mappings)
    {
        QueueImpl& queue = QueueImpl::of(*this);
        core::DeviceCore& core = queue.device->core;
        core::mapTiles(core, queue.type, mappings);
        // UpdateTileMappings is a queue operation, ordered after the queue's earlier work and before its later work, as the
        // reference orders a mapping. It leaves the tile's memory as it was: the first use of a newly mapped tile is ordered
        // by the barrier VU-rojo-sparse-0014 requires, which the mapping does not issue. Grouping does not reorder anything
        // that matters, because no two elements name one tile (VU-rojo-sparse-0012).
        for (const MappingGroup& group : groupMappings(core, mappings))
        {
            updateMappings(queue, group);
        }
    }
} // namespace rojoRHI
