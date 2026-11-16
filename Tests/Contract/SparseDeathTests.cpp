//----------------------------------------------------------------------------------------------------------------------
/// @file SparseDeathTests.cpp
/// @brief Death tests for the valid-usage rules of sparse resources in chapter 13 and VU-rojo-device-0009.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Backend.h"
#include "Support/Death.h"
#include "Support/SparseKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using rojoRHI::test::SparseKit;

namespace {
    //==================================================================================================================
    Texture plainTexture(Device device)
    {
        return *device.createTexture(TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 256, 256, 1 }, .usage = TextureUsage::Sampled });
    }

    //==================================================================================================================
    /// A sparse texture of one 128 by 128 tile and a single mip, which therefore has no mip tail.
    Texture oneTileTexture(Device device)
    {
        return *device.createTexture(TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 128, 128, 1 }, .usage = TextureUsage::Sparse });
    }

    //==================================================================================================================
    void mapOne(SparseKit& kit, const TileMapping& mapping)
    {
        kit.queue.mapTiles(std::array{ mapping });
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0001 the tile shape of a texture without Sparse usage is asked", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0001", [] {
        Device device = *createDevice();
        (void)device.tileShape(plainTexture(device));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0001 the tile shape of a buffer without Sparse usage is asked", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0001", [] {
        Device device = *createDevice();
        (void)device.tileShape(*device.createBuffer(BufferDesc{ .size = 65536, .memory = Memory::Gpu }));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 the tile shape of a swapchain image is asked", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        Device device = *createDevice();
        Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(), .label = "test.swapchain" });
        device.beginFrame();
        (void)device.tileShape(swapchain.acquire());
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 a swapchain image is mapped", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        SparseKit kit;
        Swapchain swapchain = *kit.device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(), .label = "test.swapchain" });
        kit.device.beginFrame();
        TileMapping mapping = kit.firstTile();
        mapping.texture = swapchain.acquire();
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0002 a mapping names neither a texture nor a buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0002", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.texture = {};
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0002 a mapping names a texture and a buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0002", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.buffer = kit.buffer;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0003 a mapping names a texture without Sparse usage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0003", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.texture = plainTexture(kit.device);
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0003 a mapping names a buffer without Sparse usage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0003", [] {
        SparseKit kit;
        TileMapping mapping = kit.bufferTiles();
        mapping.buffer = *kit.device.createBuffer(BufferDesc{ .size = 65536, .memory = Memory::Gpu });
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0004 a region starts off the tile grid", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0004", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.region.offset.x = 1;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0004 a region ends inside a tile short of the level's edge", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0004", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.region.extent.width = 1;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0004 a region lies in the mip tail", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0004", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.region.mip = kit.shape.mipTailFirst;
        mapping.region.extent = { 1, 1, 1 };
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0004 a region reaches past its level", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0004", [] {
        SparseKit kit;
        TileMapping mapping = kit.wholeLevel();
        mapping.region.offset.x = kit.shape.width;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0004 a region names layers past the texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0004", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.region.layer = SparseKit::kLayers;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0005 a mip tail is mapped on a texture without one", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0005", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.texture = oneTileTexture(kit.device);
        mapping.mipTail = true;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0005 a mip tail is mapped for layers past the texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0005", [] {
        SparseKit kit;
        TileMapping mapping = kit.tail();
        mapping.region.layers = SparseKit::kLayers + 1;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0006 a buffer mapping sets mipTail", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0006", [] {
        SparseKit kit;
        TileMapping mapping = kit.bufferTiles();
        mapping.mipTail = true;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0007 a buffer range starts off the tile grid", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0007", [] {
        SparseKit kit;
        TileMapping mapping = kit.bufferTiles();
        mapping.bufferOffset = 1;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0007 a buffer range is empty", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0007", [] {
        SparseKit kit;
        TileMapping mapping = kit.bufferTiles();
        mapping.bufferSize = 0;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0007 a buffer range ends inside a tile short of the buffer's end", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0007", [] {
        SparseKit kit;
        TileMapping mapping = kit.bufferTiles();
        mapping.bufferSize = 1;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0007 a buffer range reaches past the buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0007", [] {
        SparseKit kit;
        mapOne(kit, kit.bufferTiles(SparseKit::kBufferTiles + 1));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0008 a mapping names no heap", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0008", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.heap = {};
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0008 a heap offset is not a multiple of the tile bytes", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0008", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.heapOffset = 1;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0008 a mapped tile lies past the end of the heap", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0008", [] {
        SparseKit kit;
        mapOne(kit, kit.wholeLevel((SparseKit::kHeapTiles - 1) * kit.shape.bytes));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0009 tiles are mapped from an Upload heap", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0009", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.heap = *kit.device.createHeap({ .size = 65536, .memory = Memory::Upload });
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0010 an unmap names a heap", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0010", [] {
        SparseKit kit;
        TileMapping mapping = kit.firstTile();
        mapping.unmap = true;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0010 an unmap gives a heap offset", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0010", [] {
        SparseKit kit;
        TileMapping mapping = SparseKit::removal(kit.firstTile());
        mapping.heapOffset = kit.shape.bytes;
        mapOne(kit, mapping);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0006 the third element of a call of three sets mipTail on a buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0006", [] {
        SparseKit kit;
        TileMapping broken = kit.bufferTiles();
        broken.mipTail = true;
        TileMapping second = kit.firstTile();
        second.region.offset.x = kit.shape.width;
        kit.queue.mapTiles(std::array{ kit.firstTile(), second, broken });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0012 two elements map the same tile", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0012", [] {
        SparseKit kit;
        TileMapping other = kit.firstTile();
        other.heapOffset = kit.shape.bytes;
        kit.queue.mapTiles(std::array{ kit.firstTile(), other });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0012 a region and an unmap name the same tile", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0012", [] {
        SparseKit kit;
        kit.queue.mapTiles(std::array{ kit.wholeLevel(), SparseKit::removal(kit.firstTile()) });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0012 two elements map the same range of a buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0012", [] {
        SparseKit kit;
        kit.queue.mapTiles(std::array{ kit.bufferTiles(2), kit.bufferTiles(1, 8 * kit.shape.bytes) });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0009 a heap with a mapped tile is destroyed", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0009", [] {
        SparseKit kit;
        mapOne(kit, kit.firstTile());
        kit.device.destroy(kit.heap);
    });
}
