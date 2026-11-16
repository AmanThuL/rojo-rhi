//----------------------------------------------------------------------------------------------------------------------
/// @file SparseKit.cpp
/// @brief Implements the helper that builds a device, a heap and sparse resources, and the valid mappings of them.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/SparseKit.h"

namespace rojoRHI::test {
    //==================================================================================================================
    SparseKit::SparseKit()
    {
        device = *createDevice();
        queue = device.queue(QueueType::Graphics);
        // A tile is 64 KiB on every backend that takes this path; the heap and buffer are sized in tiles through the shape.
        buffer =
            *device.createBuffer(BufferDesc{ .size = kBufferTiles * 65536, .memory = Memory::Gpu, .usage = BufferUsage::Sparse, .label = "kit.sparse.buffer" });
        bufShape = device.tileShape(buffer);
        heap = *device.createHeap(HeapDesc{ .size = kHeapTiles * 65536, .memory = Memory::Gpu, .label = "kit.sparse.heap" });
        texture = *device.createTexture(
            TextureDesc{ .type = TextureType::Tex2DArray,
                         .format = Format::RGBA8Unorm,
                         .extent = { kEdge, kEdge, 1 },
                         .mips = kMips,
                         .layers = kLayers,
                         .usage = TextureUsage::Sampled | TextureUsage::Sparse,
                         .label = "kit.sparse.texture" }
        );
        shape = device.tileShape(texture);
    }

    //==================================================================================================================
    TileMapping SparseKit::firstTile() const
    {
        return TileMapping{ .texture = texture, .region = { .extent = { shape.width, shape.height, 1 } }, .heap = heap };
    }

    //==================================================================================================================
    TileMapping SparseKit::wholeLevel(uint64_t heapOffset) const
    {
        return TileMapping{ .texture = texture, .region = { .extent = { kEdge, kEdge, 1 } }, .heap = heap, .heapOffset = heapOffset };
    }

    //==================================================================================================================
    TileMapping SparseKit::tail(uint64_t heapOffset) const
    {
        return TileMapping{ .texture = texture, .region = { .mip = shape.mipTailFirst }, .mipTail = true, .heap = heap, .heapOffset = heapOffset };
    }

    //==================================================================================================================
    TileMapping SparseKit::bufferTiles(uint64_t tiles, uint64_t heapOffset) const
    {
        return TileMapping{ .buffer = buffer, .bufferSize = tiles * bufShape.bytes, .heap = heap, .heapOffset = heapOffset };
    }

    //==================================================================================================================
    TileMapping SparseKit::removal(TileMapping mapping)
    {
        mapping.heap = {};
        mapping.heapOffset = 0;
        mapping.unmap = true;
        return mapping;
    }
} // namespace rojoRHI::test
