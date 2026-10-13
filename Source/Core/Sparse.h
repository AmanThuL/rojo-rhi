//----------------------------------------------------------------------------------------------------------------------
/// @file Sparse.h
/// @brief Declares the tile shapes, the tile mapping state and the checks of the sparse rules of chapter 13, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Handles.h>
#include <rojoRHI/TileMapping.h>
#include <rojoRHI/TileShape.h>

#include "Core/DeviceCore.h"
#include "Core/Records.h"

#include <span>

namespace rojoRHI::core {
    /// The heap bytes of one tile of every sparse resource: the fixed 64 KiB tile chapter 13 names for Metal 4 and D3D12.
    inline constexpr uint32_t kTileBytes = 65536;

    /// Returns the tile shape of the texture `record` describes, from the extents the backend registered with
    /// `DeviceCore::setTileExtents`. It checks nothing; `tileShape` is the checked entry.
    TileShape textureTileShape(const DeviceCore& core, const TextureRecord& record);

    /// Returns the tile shape of a sparse buffer: `kTileBytes` wide, one row and one slice, with no mip tail.
    TileShape bufferTileShape();

    /// Returns the tile shape of `texture` after checking VU-rojo-present-0012 and VU-rojo-sparse-0001 (VU-rojo-core-0001 to
    /// 0003 for the handle).
    TileShape tileShape(DeviceCore& core, Texture texture);

    /// Returns the tile shape of `buffer` after checking VU-rojo-sparse-0001 (VU-rojo-core-0001 to 0003 for the handle).
    TileShape tileShape(DeviceCore& core, Buffer buffer);

    /// Applies `mappings` in order to the mapping state of the resources they name and to the mapped-tile count of each heap.
    /// Debug builds first check every element (VU-rojo-present-0012, VU-rojo-sparse-0002 to 0012; VU-rojo-core-0001 to 0003
    /// for the handles); an invalid element reports its own rule, whatever the call length. Release applies without checking.
    void mapTiles(DeviceCore& core, std::span<const TileMapping> mappings);

    /// Drops the mapping `tiles` of a sparse resource that was destroyed: each tile no longer counts against its heap.
    void releaseTiles(DeviceCore& core, const TileMap& tiles);
} // namespace rojoRHI::core
