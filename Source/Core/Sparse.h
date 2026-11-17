//----------------------------------------------------------------------------------------------------------------------
/// @file Sparse.h
/// @brief Declares the tile shapes, the tile mapping state and the checks of the sparse rules of chapter 13, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Records.h"

#include <span>

namespace rojoRHI::core {
    // Chapter 13's fixed tile for Metal 4 and D3D12, for every sparse resource.
    inline constexpr uint32_t kTileBytes = 65536;

    // Unchecked; `tileShape` is the checked entry. `record.tiles` when the backend measured one at creation, otherwise
    // computed from the extents registered with `DeviceCore::setTileExtents`.
    TileShape textureTileShape(const DeviceCore& core, const TextureRecord& record);

    TileShape bufferTileShape();

    TileShape tileShape(DeviceCore& core, Texture texture);

    TileShape tileShape(DeviceCore& core, Buffer buffer);

    // Debug checks every element first, each reporting its own rule; a validating device's tracker then judges the
    // changes against other queues (VU-rojo-sparse-0013). It counts no native call; the backend counts its own.
    void mapTiles(DeviceCore& core, QueueType queue, std::span<const TileMapping> mappings);

    void releaseTiles(DeviceCore& core, const TileMap& tiles);
} // namespace rojoRHI::core
