//----------------------------------------------------------------------------------------------------------------------
/// @file SparseTests.cpp
/// @brief Tests the reference behaviour of sparse resources (chapter 13) that every backend shares.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/SparseKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using rojoRHI::test::SparseKit;

//======================================================================================================================
TEST_CASE("a sparse buffer's tile shape counts bytes in one row", "[contract]")
{
    SparseKit kit;
    CHECK(kit.bufShape.width == kit.bufShape.bytes);
    CHECK(kit.bufShape.height == 1);
    CHECK(kit.bufShape.depth == 1);
    CHECK(kit.bufShape.mipTailFirst == 0);
    CHECK(kit.bufShape.mipTailBytes == 0);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a sparse texture's tile shape has extents, bytes and a tail inside its mip chain", "[contract]")
{
    SparseKit kit;
    CHECK(kit.shape.width > 0);
    CHECK(kit.shape.height > 0);
    CHECK(kit.shape.depth > 0);
    CHECK(kit.shape.bytes > 0);
    CHECK(kit.shape.mipTailFirst <= SparseKit::kMips);
    CHECK((kit.shape.mipTailFirst == SparseKit::kMips) == (kit.shape.mipTailBytes == 0));
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a texture too small for a tail reports its mip count as the first tail level", "[contract]")
{
    Device device = *createDevice();
    const Texture texture = *device.createTexture(TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 1024, 1024, 1 }, .usage = TextureUsage::Sparse });
    const TileShape shape = device.tileShape(texture);
    CHECK(shape.mipTailFirst == 1);
    CHECK(shape.mipTailBytes == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an empty span of mappings does nothing", "[contract]")
{
    SparseKit kit;
    kit.queue.mapTiles({});
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a mapped heap is destroyed after its tiles are unmapped", "[contract]")
{
    SparseKit kit;
    const TileMapping mapping = kit.firstTile();
    kit.queue.mapTiles(std::array{ mapping });
    kit.queue.mapTiles(std::array{ SparseKit::removal(mapping) });
    kit.device.destroy(kit.heap);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("destroying a sparse texture unmaps its tiles so the heap can be destroyed", "[contract]")
{
    SparseKit kit;
    kit.queue.mapTiles(std::array{ kit.wholeLevel() });
    kit.device.destroy(kit.texture);
    kit.device.destroy(kit.heap);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("destroying a sparse buffer unmaps its tiles so the heap can be destroyed", "[contract]")
{
    SparseKit kit;
    kit.queue.mapTiles(std::array{ kit.bufferTiles(SparseKit::kBufferTiles) });
    kit.device.destroy(kit.buffer);
    kit.device.destroy(kit.heap);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a tile maps again onto other heap memory without being unmapped", "[contract]")
{
    SparseKit kit;
    kit.queue.mapTiles(std::array{ kit.firstTile() });
    TileMapping again = kit.firstTile();
    again.heapOffset = kit.shape.bytes;
    kit.queue.mapTiles(std::array{ again });
    kit.queue.mapTiles(std::array{ SparseKit::removal(again) });
    kit.device.destroy(kit.heap);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("one call maps a region, a mip tail and a buffer range together", "[contract]")
{
    SparseKit kit;
    if (kit.shape.mipTailFirst >= SparseKit::kMips)
    {
        kit.device.destroy(kit.texture);
        kit.device.destroy(kit.buffer);
        kit.device.destroy(kit.heap);
        destroyDevice(kit.device);
        SKIP("the device packs no mip tail for the sparse texture, so there is no tail to map");
    }
    const uint64_t regionTiles = 16;
    const std::array mappings = { kit.wholeLevel(0), kit.tail(regionTiles * kit.shape.bytes), kit.bufferTiles(2, 20 * kit.shape.bytes) };
    kit.queue.mapTiles(mappings);
    kit.device.destroy(kit.texture);
    kit.device.destroy(kit.buffer);
    kit.device.destroy(kit.heap);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("unmapping a tile that was never mapped is accepted", "[contract]")
{
    SparseKit kit;
    kit.queue.mapTiles(std::array{ SparseKit::removal(kit.firstTile()) });
    kit.device.destroy(kit.heap);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("any queue type may map tiles", "[contract]")
{
    SparseKit kit;
    kit.device.queue(QueueType::Compute).mapTiles(std::array{ kit.firstTile() });
    kit.device.queue(QueueType::Copy).mapTiles(std::array{ SparseKit::removal(kit.firstTile()) });
    destroyDevice(kit.device);
}
