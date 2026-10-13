//----------------------------------------------------------------------------------------------------------------------
/// @file SparseCoreTests.cpp
/// @brief Tests the mapping the core keeps for each sparse resource and the tile count of each heap.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Support/SparseKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using rojoRHI::test::SparseKit;

namespace {
    //==================================================================================================================
    uint64_t mappedFrom(Device device, Heap heap)
    {
        return core::coreOf(device).pool<Heap>().lookup(heap).record->mappedTiles;
    }

    //==================================================================================================================
    size_t tilesOf(Device device, Texture texture)
    {
        return core::coreOf(device).pool<Texture>().lookup(texture).record->mapped.size();
    }

    //==================================================================================================================
    size_t tilesOf(Device device, Buffer buffer)
    {
        return core::coreOf(device).pool<Buffer>().lookup(buffer).record->mapped.size();
    }
} // namespace

//======================================================================================================================
TEST_CASE("a region maps one tile per heap tile and each tile counts against the heap", "[core]")
{
    SparseKit kit;
    kit.queue.mapTiles(std::array{ kit.wholeLevel() });
    const uint64_t perAxis = SparseKit::kEdge / kit.shape.width;
    CHECK(tilesOf(kit.device, kit.texture) == perAxis * (SparseKit::kEdge / kit.shape.height));
    CHECK(mappedFrom(kit.device, kit.heap) == tilesOf(kit.device, kit.texture));
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a mip tail is one tile per layer in the region", "[core]")
{
    SparseKit kit;
    TileMapping tail = kit.tail();
    tail.region.layers = SparseKit::kLayers;
    kit.queue.mapTiles(std::array{ tail });
    CHECK(tilesOf(kit.device, kit.texture) == SparseKit::kLayers);
    CHECK(mappedFrom(kit.device, kit.heap) == SparseKit::kLayers);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("mapping a tile again moves its count and unmapping removes it", "[core]")
{
    SparseKit kit;
    const Heap other = *kit.device.createHeap({ .size = 4 * 65536, .memory = Memory::Gpu });
    kit.queue.mapTiles(std::array{ kit.firstTile() });
    TileMapping moved = kit.firstTile();
    moved.heap = other;
    kit.queue.mapTiles(std::array{ moved });
    CHECK(mappedFrom(kit.device, kit.heap) == 0);
    CHECK(mappedFrom(kit.device, other) == 1);
    CHECK(tilesOf(kit.device, kit.texture) == 1);
    kit.queue.mapTiles(std::array{ SparseKit::removal(moved) });
    CHECK(mappedFrom(kit.device, other) == 0);
    CHECK(tilesOf(kit.device, kit.texture) == 0);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a buffer range maps one tile per heap tile", "[core]")
{
    SparseKit kit;
    kit.queue.mapTiles(std::array{ kit.bufferTiles(3) });
    CHECK(tilesOf(kit.device, kit.buffer) == 3);
    CHECK(mappedFrom(kit.device, kit.heap) == 3);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("destroying a sparse resource drops its tiles from the heap count at the call", "[core]")
{
    SparseKit kit;
    kit.queue.mapTiles(std::array{ kit.bufferTiles(2), kit.wholeLevel(4 * kit.shape.bytes) });
    const uint64_t texTiles = tilesOf(kit.device, kit.texture);
    CHECK(mappedFrom(kit.device, kit.heap) == 2 + texTiles);
    kit.device.destroy(kit.texture);
    CHECK(mappedFrom(kit.device, kit.heap) == 2);
    kit.device.destroy(kit.buffer);
    CHECK(mappedFrom(kit.device, kit.heap) == 0);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a partial unmap leaves the other tiles mapped", "[core]")
{
    SparseKit kit;
    kit.queue.mapTiles(std::array{ kit.wholeLevel() });
    const uint64_t before = tilesOf(kit.device, kit.texture);
    kit.queue.mapTiles(std::array{ SparseKit::removal(kit.firstTile()) });
    CHECK(tilesOf(kit.device, kit.texture) == before - 1);
    CHECK(mappedFrom(kit.device, kit.heap) == before - 1);
    destroyDevice(kit.device);
}
