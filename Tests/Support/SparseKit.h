//----------------------------------------------------------------------------------------------------------------------
/// @file SparseKit.h
/// @brief Declares the helper that builds a device, a heap and sparse resources, and the valid mappings of them.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstdint>

namespace rojoRHI::test {
    // The mappings it returns are valid. It never destroys anything, so a death test can abort at any point.
    class SparseKit
    {
      public:
        static constexpr uint64_t kBufferTiles = 4;
        static constexpr uint64_t kHeapTiles = 64;
        static constexpr uint32_t kEdge = 512;
        // Down to one texel.
        static constexpr uint32_t kMips = 10;
        static constexpr uint32_t kLayers = 2;

        SparseKit();

        TileMapping firstTile() const;

        TileMapping wholeLevel(uint64_t heapOffset = 0) const;

        TileMapping tail(uint64_t heapOffset = 0) const;

        TileMapping bufferTiles(uint64_t tiles = 1, uint64_t heapOffset = 0) const;

        static TileMapping removal(TileMapping mapping);

        Device device;
        Queue queue;
        Heap heap;
        Buffer buffer;
        Texture texture;
        TileShape shape;
        TileShape bufShape;
    };
} // namespace rojoRHI::test
