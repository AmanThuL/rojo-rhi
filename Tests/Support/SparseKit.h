//----------------------------------------------------------------------------------------------------------------------
/// @file SparseKit.h
/// @brief Declares the helper that builds a device, a heap and sparse resources, and the valid mappings of them.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstdint>

namespace rojoRHI::test {
    /// A device with a graphics queue, a `Memory::Gpu` heap, a sparse buffer and a sparse 2D array texture with a mip tail, and
    /// the arguments of valid mappings of them. It never destroys anything, so a death test can abort at any point.
    class SparseKit
    {
      public:
        /// Tiles the buffer spans.
        static constexpr uint64_t kBufferTiles = 4;
        /// Tiles the heap holds.
        static constexpr uint64_t kHeapTiles = 64;
        /// Edge of the texture's mip 0, in texels.
        static constexpr uint32_t kEdge = 512;
        /// Mip levels of the texture, down to one texel.
        static constexpr uint32_t kMips = 10;
        /// Array layers of the texture.
        static constexpr uint32_t kLayers = 2;

        /// Creates the device and the objects.
        SparseKit();

        /// Returns a valid mapping of the first tile of mip 0, layer 0 of the texture onto the heap's first tile.
        TileMapping firstTile() const;

        /// Returns a valid mapping of the whole of mip 0, layer 0 of the texture onto the heap from `heapOffset`.
        TileMapping wholeLevel(uint64_t heapOffset = 0) const;

        /// Returns a valid mapping of the mip tail of layer 0 of the texture onto the heap from `heapOffset`.
        TileMapping tail(uint64_t heapOffset = 0) const;

        /// Returns a valid mapping of the first `tiles` tiles of the buffer onto the heap from `heapOffset`.
        TileMapping bufferTiles(uint64_t tiles = 1, uint64_t heapOffset = 0) const;

        /// Returns the removal of the mapping `mapping` describes.
        static TileMapping removal(TileMapping mapping);

        Device device;      ///< The device.
        Queue queue;        ///< Its graphics queue.
        Heap heap;          ///< A `Memory::Gpu` heap of `kHeapTiles` tiles.
        Buffer buffer;      ///< A sparse buffer of `kBufferTiles` tiles.
        Texture texture;    ///< A sparse `RGBA8Unorm` texture of `kEdge` square texels, `kMips` mips and `kLayers` layers.
        TileShape shape;    ///< The tile shape of `texture`.
        TileShape bufShape; ///< The tile shape of `buffer`.
    };
} // namespace rojoRHI::test
