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
    /// One resource region of an `UpdateTileMappings` call and the heap range it maps from: the range starts at `heapStart`
    /// and spans `size.NumTiles` tiles, walked in the region's x, then y, then z order.
    struct TileRegion
    {
        D3D12_TILED_RESOURCE_COORDINATE start = {}; ///< The region's first tile and the subresource that holds it.
        D3D12_TILE_REGION_SIZE size = {};           ///< The region's tiles: a box for a texture region, a count otherwise.
        UINT heapStart = 0;                         ///< The first heap tile of the range; zero for an unmap.
    };

    /// Creates the reserved resource of a Sparse buffer whose committed form `desc` describes, in
    /// `D3D12_BARRIER_LAYOUT_UNDEFINED`, its width rounded up to whole 64 KiB tiles so the last tile maps like any other.
    /// `label` names the buffer in the error of a refused creation; the caller names the resource.
    Result<Com<ID3D12Resource>> createReservedBuffer(DeviceImpl& impl, const D3D12_RESOURCE_DESC1& desc, std::string_view label);

    /// Creates the reserved resource of a Sparse texture of `desc`, whose committed form `native` describes, castable to
    /// `castable`, in `D3D12_BARRIER_LAYOUT_UNDEFINED` with the 64 KiB undefined-swizzle layout. A one-dimensional type, a 3D
    /// texture below tiled-resources tier 3 and a format the device does not report sparse fail with `Unsupported`; a
    /// creation the runtime refuses fails with its HRESULT. `label` names the texture in the error; the caller names it.
    Result<Com<ID3D12Resource>> createReservedTexture(
        DeviceImpl& impl, const TextureDesc& desc, const D3D12_RESOURCE_DESC1& native, std::span<const DXGI_FORMAT> castable, std::string_view label
    );

    /// Returns the tile shape of `resource`, a reserved texture of `mips` mips on `device`, from `GetResourceTiling`: the
    /// extents of `D3D12_TILE_SHAPE`, 64 KiB tiles, the first packed mip as the first tail level (`mips` when no mip is
    /// packed) and the packed mips' tiles of one layer as the tail's bytes.
    TileShape readTileShape(ID3D12Device12& device, ID3D12Resource& resource, uint32_t mips);

    /// Returns the regions a texture region or mip tail `mapping` of `texture` maps, one per layer. A region's layer is the
    /// box of its tiles at subresource `mip + layer * mips`; a tail's layer is its packed tiles, counted from x 0 of the
    /// first packed mip of that layer. Each layer's heap range follows the previous layer's tiles from `heapOffset`.
    std::vector<TileRegion> tileCoordinates(const core::TextureRecord& texture, const TileMapping& mapping);

    /// Returns the region a buffer range `mapping` maps: its tiles counted from the tile at `bufferOffset`, the last partial
    /// tile included, from the heap tile at `heapOffset`.
    TileRegion tileCoordinates(const TileMapping& mapping);
} // namespace rojoRHI::d3d12
