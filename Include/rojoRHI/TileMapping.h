//----------------------------------------------------------------------------------------------------------------------
/// @file TileMapping.h
/// @brief Declares one change to the mapping of a sparse resource.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Handles.h>
#include <rojoRHI/TextureRegion.h>

#include <cstdint>

namespace rojoRHI {
    /// One change to the mapping of a texture region, a texture's mip tail, or a buffer byte range, applied by
    /// `Queue::mapTiles`. Exactly one of `texture` and `buffer` is set (VU-rojo-sparse-0002) and it was created with `Sparse`
    /// usage (VU-rojo-sparse-0003). A texture region lies in one mip level below the tile shape's `mipTailFirst`, aligned to
    /// the tile shape (VU-rojo-sparse-0004); with `mipTail` only the layer range of `region` is read and the texture has a tail
    /// (VU-rojo-sparse-0005); `mipTail` is false for a buffer (VU-rojo-sparse-0006). A buffer range is a multiple of the tile
    /// size or reaches the buffer's end and lies inside it (VU-rojo-sparse-0007). Without `unmap`, `heap` is a `Memory::Gpu`
    /// heap (VU-rojo-sparse-0009), `heapOffset` is a multiple of the tile bytes and every mapped tile lies inside the heap
    /// (VU-rojo-sparse-0008); with `unmap`, `heap` is null and `heapOffset` is zero (VU-rojo-sparse-0010). The tiles of a
    /// region map onto consecutive heap tiles in order of x, then y, then z, then layer; mip tails map one after another, one
    /// per layer, each `TileShape::mipTailBytes` long; a buffer range maps onto consecutive heap bytes.
    struct TileMapping
    {
        /// Sparse texture to map, or null when `buffer` is set.
        Texture texture = {};
        /// Texels of one mip level, over a range of layers; with `mipTail` only its layer range is read.
        TextureRegion region;
        /// Maps the mip tail of each of `region`'s layers instead of a region.
        bool mipTail = false;
        /// Sparse buffer to map, or null when `texture` is set.
        Buffer buffer = {};
        /// Byte offset of the range of `buffer` to map.
        uint64_t bufferOffset = 0;
        /// Byte size of the range of `buffer` to map.
        uint64_t bufferSize = 0;
        /// Heap supplying the memory; null with `unmap`.
        Heap heap = {};
        /// Byte offset of the first tile in `heap`.
        uint64_t heapOffset = 0;
        /// Removes the mapping of the named tiles instead of adding one.
        bool unmap = false;
    };
} // namespace rojoRHI
