//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Sparse.h
/// @brief Declares the D3D12 reserved resources, their tile shapes and the tile coordinates of an UpdateTileMappings call.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"
#include "D3D12Common.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
    // The heap range starts at `heapStart` and spans `size.NumTiles` tiles, walked in the region's x, then y, then z
    // order.
    struct TileRegion
    {
        D3D12_TILED_RESOURCE_COORDINATE start = {};
        D3D12_TILE_REGION_SIZE size = {}; // A box for a texture region, a count otherwise.
        UINT heapStart = 0;               // Zero for an unmap.
    };

    // In LAYOUT_UNDEFINED, its width rounded up to whole 64 KiB tiles so the last tile maps like any other. `label`
    // names the buffer in the error of a refused creation; the caller names the resource.
    Result<Com<ID3D12Resource>> createReservedBuffer(DeviceImpl& impl, const D3D12_RESOURCE_DESC1& desc, std::string_view label);

    // In LAYOUT_UNDEFINED with the 64 KiB undefined-swizzle layout. A one-dimensional type, a 3D texture below
    // tiled-resources tier 3 and a format the device does not report sparse are `Unsupported`; a creation the runtime
    // refuses fails with its HRESULT. `label` names the texture in the error; the caller names the resource.
    Result<Com<ID3D12Resource>> createReservedTexture(
        DeviceImpl& impl, const TextureDesc& desc, const D3D12_RESOURCE_DESC1& native, std::span<const DXGI_FORMAT> castable, std::string_view label
    );

    // From `GetResourceTiling`: the D3D12_TILE_SHAPE extents, 64 KiB tiles, the first packed mip as the first tail
    // level (`mips` when no mip is packed) and the packed mips' tiles of one layer as the tail's bytes.
    TileShape readTileShape(ID3D12Device12& device, ID3D12Resource& resource, uint32_t mips);

    // One per layer. A region's layer is the box of its tiles at subresource `mip + layer * mips`; a tail's layer is
    // its packed tiles, counted from x 0 of that layer's first packed mip. Each layer's heap range follows the previous
    // layer's tiles from `heapOffset`.
    std::vector<TileRegion> tileCoordinates(const core::TextureRecord& texture, const TileMapping& mapping);

    // Its tiles counted from the tile at `bufferOffset`, the last partial tile included, from the heap tile at
    // `heapOffset`.
    TileRegion tileCoordinates(const TileMapping& mapping);
} // namespace rojoRHI::d3d12
