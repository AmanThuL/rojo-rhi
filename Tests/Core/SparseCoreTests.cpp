//----------------------------------------------------------------------------------------------------------------------
/// @file SparseCoreTests.cpp
/// @brief Tests the mapping the core keeps for each sparse resource and the tile count of each heap.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Records.h"
#include "Core/Sparse.h"
#include "Support/Death.h"
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

    /// A tile shape unlike the standard one of `measuredTexture`: its tail starts a mip earlier and spans two tiles, as a 3D
    /// texture's tail can on a backend that measures it.
    constexpr TileShape kMeasuredShape = { .width = 32, .height = 32, .depth = 16, .bytes = 65536, .mipTailFirst = 2, .mipTailBytes = 2 * 65536 };

    //==================================================================================================================
    /// Creates a sparse 3D texture on `device` and records `kMeasuredShape` as the shape its backend measured.
    Texture measuredTexture(Device device)
    {
        const Texture texture = *device.createTexture(
            TextureDesc{ .type = TextureType::Tex3D,
                         .format = Format::RGBA8Unorm,
                         .extent = { 128, 128, 64 },
                         .mips = 4,
                         .usage = TextureUsage::Sampled | TextureUsage::Sparse,
                         .label = "core.sparse.measured" }
        );
        // The test stands in for the backend after creation; nothing reads the record concurrently here.
        core::coreOf(device).pool<Texture>().update(texture, [](core::TextureRecord& record) { record.tiles = kMeasuredShape; });
        return texture;
    }

    //==================================================================================================================
    /// Returns a mapping of the mip tail of `texture` onto `heap` at `heapOffset`.
    TileMapping measuredTail(Texture texture, Heap heap, uint64_t heapOffset)
    {
        return TileMapping{ .texture = texture, .region = { .mip = kMeasuredShape.mipTailFirst }, .mipTail = true, .heap = heap, .heapOffset = heapOffset };
    }

    //==================================================================================================================
    bool sameShape(const TileShape& left, const TileShape& right)
    {
        return left.width == right.width && left.height == right.height && left.depth == right.depth && left.bytes == right.bytes &&
               left.mipTailFirst == right.mipTailFirst && left.mipTailBytes == right.mipTailBytes;
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

//======================================================================================================================
TEST_CASE("a texture record with a measured tile shape reports it unchanged", "[core]")
{
    Device device = *createDevice();
    const Texture texture = measuredTexture(device);
    CHECK(sameShape(core::tileShape(core::coreOf(device), texture), kMeasuredShape));
    // A heap of exactly the measured tail holds it.
    const Heap heap = *device.createHeap({ .size = kMeasuredShape.mipTailBytes, .memory = Memory::Gpu });
    device.queue(QueueType::Graphics).mapTiles(std::array{ measuredTail(texture, heap, 0) });
    CHECK(tilesOf(device, texture) == 1);
    CHECK(mappedFrom(device, heap) == 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0008 a measured tail of two tiles does not fit the last tile of a heap", "[core][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0008", [] {
        Device device = *createDevice();
        const Texture texture = measuredTexture(device);
        const Heap heap = *device.createHeap({ .size = kMeasuredShape.mipTailBytes, .memory = Memory::Gpu });
        device.queue(QueueType::Graphics).mapTiles(std::array{ measuredTail(texture, heap, kMeasuredShape.bytes) });
    });
}
