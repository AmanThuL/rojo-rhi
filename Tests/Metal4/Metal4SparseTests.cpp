//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4SparseTests.cpp
/// @brief Tests Metal 4 placement sparse resources against the native objects: capabilities, tile shapes and tile mappings.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Formats.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Metal4Device.h"
#include "Metal4Formats.h"
#include "Metal4Sparse.h"
#include "Metal4TextureDescriptor.h"
#include "Support/SparseKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Metal/Metal.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace rojoRHI;
using rojoRHI::test::SparseKit;

namespace {
    /// Heap bytes of one tile.
    constexpr uint64_t kTile = 65536;

    //==================================================================================================================
    /// The native texture `texture` wraps.
    MTL::Texture* nativeOf(Device device, Texture texture)
    {
        return static_cast<MTL::Texture*>(DeviceImpl::of(device).core.pool<Texture>().native(texture));
    }

    //==================================================================================================================
    /// The native buffer `buffer` wraps.
    MTL::Buffer* nativeOf(Device device, Buffer buffer)
    {
        return static_cast<MTL::Buffer*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
    }

    //==================================================================================================================
    /// The native heap `heap` wraps.
    MTL::Heap* nativeOf(Device device, Heap heap)
    {
        return static_cast<MTL::Heap*>(DeviceImpl::of(device).core.pool<Heap>().native(heap));
    }

    //==================================================================================================================
    /// Runs `count` empty frames on `device`.
    void runFrames(Device device, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            device.beginFrame();
            device.endFrame();
        }
    }

    //==================================================================================================================
    /// Returns the groups the Metal 4 backend issues for `mappings` on `device`.
    std::vector<metal4::MappingGroup> groupsOf(Device device, std::span<const TileMapping> mappings)
    {
        const std::vector<metal4::MappingTarget> targets = metal4::resolveMappings(DeviceImpl::of(device).core, mappings);
        return metal4::groupMappings(mappings, targets);
    }

    //==================================================================================================================
    /// Checks that `operation` is a texture operation of `mode` on `level` and `slice`, covering the tiles from (`x`, `y`, `z`)
    /// of `width` by `height` by `depth` tiles, from heap tile `heapTile`.
    void checkOperation(
        const MTL4::UpdateSparseTextureMappingOperation& operation,
        MTL::SparseTextureMappingMode mode,
        uint32_t level,
        uint32_t slice,
        std::array<uint32_t, 3> origin,
        std::array<uint32_t, 3> size,
        uint64_t heapTile
    )
    {
        CHECK(operation.mode == mode);
        CHECK(operation.textureLevel == level);
        CHECK(operation.textureSlice == slice);
        CHECK(operation.textureRegion.origin.x == origin[0]);
        CHECK(operation.textureRegion.origin.y == origin[1]);
        CHECK(operation.textureRegion.origin.z == origin[2]);
        CHECK(operation.textureRegion.size.width == size[0]);
        CHECK(operation.textureRegion.size.height == size[1]);
        CHECK(operation.textureRegion.size.depth == size[2]);
        CHECK(operation.heapOffset == heapTile);
    }

    //==================================================================================================================
    /// Creates a sparse texture of `type` in RGBA8Unorm with `extent`, `mips` and `layers`, labelled `label`.
    Texture sparseTexture(Device device, TextureType type, Extent3D extent, uint32_t mips, uint32_t layers, const char* label)
    {
        return *device.createTexture(
            TextureDesc{ .type = type,
                         .format = Format::RGBA8Unorm,
                         .extent = extent,
                         .mips = mips,
                         .layers = layers,
                         .usage = TextureUsage::Sampled | TextureUsage::Sparse,
                         .label = label }
        );
    }
} // namespace

//======================================================================================================================
TEST_CASE("the device reports placement sparse and every format it reports sparse creates a sparse 2D texture", "[metal4]")
{
    Device device = *createDevice();
    MTL::Device* native = DeviceImpl::of(device).device;
    REQUIRE(device.capabilities().placementSparse == native->supportsPlacementSparse());
    REQUIRE(device.capabilities().placementSparse);
    uint32_t reported = 0;
    for (uint32_t value = 1; value < core::kFormatCount; ++value)
    {
        const auto format = static_cast<Format>(value);
        CAPTURE(value);
        const bool expected = metal4::sparseTileExtent(native, MTL::TextureType2D, metal4::pixelFormat(format)).width != 0;
        CHECK(device.formatCapabilities(format).sparse == expected);
        if (!device.formatCapabilities(format).sparse)
        {
            continue;
        }
        ++reported;
        const Texture texture = *device.createTexture(
            TextureDesc{ .format = format, .extent = { 256, 256, 1 }, .usage = TextureUsage::Sampled | TextureUsage::Sparse, .label = "sparse.format" }
        );
        const TileShape shape = device.tileShape(texture);
        CHECK(shape.width > 0);
        CHECK(shape.height > 0);
        CHECK(shape.bytes == kTile);
        CHECK(nativeOf(device, texture)->isSparse());
        device.destroy(texture);
    }
    CHECK(reported > 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("every texture type creates a sparse texture whose tile shape is measured from its native texture", "[metal4]")
{
    struct Case
    {
        TextureType type;
        Extent3D extent;
        uint32_t mips;
        uint32_t layers;
    };
    const Case shape = GENERATE(
        Case{ TextureType::Tex1D, { 4096, 1, 1 }, 1, 1 },
        Case{ TextureType::Tex1DArray, { 4096, 1, 1 }, 1, 2 },
        Case{ TextureType::Tex2D, { 512, 512, 1 }, 10, 1 },
        Case{ TextureType::Tex2DArray, { 512, 512, 1 }, 10, 2 },
        Case{ TextureType::Tex3D, { 128, 128, 64 }, 4, 1 },
        Case{ TextureType::Cube, { 256, 256, 1 }, 3, 6 },
        Case{ TextureType::CubeArray, { 256, 256, 1 }, 3, 12 }
    );
    CAPTURE(static_cast<int>(shape.type));
    Device device = *createDevice();
    MTL::Device* native = DeviceImpl::of(device).device;
    const Texture texture = sparseTexture(device, shape.type, shape.extent, shape.mips, shape.layers, "sparse.type");
    const MTL::Texture* nativeTexture = nativeOf(device, texture);
    const MTL::Size extent = metal4::sparseTileExtent(native, metal4::textureType(shape.type, 1), MTL::PixelFormatRGBA8Unorm);
    const TileShape measured = device.tileShape(texture);
    CHECK(measured.width == extent.width);
    CHECK(measured.height == extent.height);
    CHECK(measured.depth == extent.depth);
    CHECK(measured.bytes == native->sparseTileSizeInBytes(MTL::SparsePageSize64));
    const uint32_t first = static_cast<uint32_t>(std::min<NS::UInteger>(nativeTexture->firstMipmapInTail(), shape.mips));
    CHECK(measured.mipTailFirst == first);
    CHECK(measured.mipTailBytes == (first < shape.mips ? nativeTexture->tailSizeInBytes() : 0));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the kit texture's tile shape is sparseTileSize with the native tail properties", "[metal4]")
{
    SparseKit kit;
    MTL::Device* native = DeviceImpl::of(kit.device).device;
    const MTL::Texture* texture = nativeOf(kit.device, kit.texture);
    const MTL::Size extent = native->sparseTileSize(MTL::TextureType2DArray, MTL::PixelFormatRGBA8Unorm, 1, MTL::SparsePageSize64);
    CHECK(kit.shape.width == extent.width);
    CHECK(kit.shape.height == extent.height);
    CHECK(kit.shape.depth == extent.depth);
    CHECK(kit.shape.bytes == kTile);
    CHECK(kit.shape.mipTailFirst == texture->firstMipmapInTail());
    CHECK(kit.shape.mipTailBytes == texture->tailSizeInBytes());
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a 3D texture's tail bytes come from Metal and may exceed one tile", "[metal4]")
{
    Device device = *createDevice();
    const Texture texture = sparseTexture(device, TextureType::Tex3D, { 128, 128, 64 }, 4, 1, "sparse.volume");
    const MTL::Texture* native = nativeOf(device, texture);
    const TileShape shape = device.tileShape(texture);
    CHECK(shape.mipTailFirst == native->firstMipmapInTail());
    CHECK(shape.mipTailBytes == native->tailSizeInBytes());
    CHECK(shape.mipTailBytes > kTile);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a first tail level past the last mip reports no tail", "[metal4]")
{
    Device device = *createDevice();
    // A 512-texel RGBA8 texture tiles 128 by 128, so its tail starts at mip 3; with three mips it has none.
    const Texture texture = sparseTexture(device, TextureType::Tex2D, { 512, 512, 1 }, 3, 1, "sparse.no-tail");
    const MTL::Texture* native = nativeOf(device, texture);
    REQUIRE(native->firstMipmapInTail() >= 3);
    const TileShape shape = device.tileShape(texture);
    CHECK(shape.mipTailFirst == 3);
    CHECK(shape.mipTailBytes == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a sparse buffer is created at its own length with 64 KiB pages and joins the residency set", "[metal4]")
{
    Device device = *createDevice();
    const Buffer buffer =
        *device.createBuffer(BufferDesc{ .size = 4 * kTile + 100, .memory = Memory::Gpu, .usage = BufferUsage::Sparse, .label = "sparse.odd" });
    const MTL::Buffer* native = nativeOf(device, buffer);
    CHECK(native->length() == 4 * kTile + 100);
    CHECK(native->sparseBufferTier() != MTL::BufferSparseTierNone);
    CHECK(DeviceImpl::of(device).residency.contains(native));
    // The last, partial tile maps like any other.
    const Heap heap = *device.createHeap(HeapDesc{ .size = 8 * kTile, .memory = Memory::Gpu, .label = "sparse.heap" });
    const TileMapping last{ .buffer = buffer, .bufferOffset = 4 * kTile, .bufferSize = 100, .heap = heap, .heapOffset = 7 * kTile };
    device.queue(QueueType::Graphics).mapTiles(std::array{ last });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture region maps per layer, in tiles, with each layer's heap offset after the previous layer's tiles", "[metal4]")
{
    SparseKit kit;
    // Mip 0 is 4 by 4 tiles of 128 texels; the region is the 2 by 1 tiles from tile (1, 2), over both layers, from heap tile 5.
    const TileMapping region{ .texture = kit.texture,
                              .region = { .mip = 0, .layer = 0, .layers = 2, .offset = { 128, 256, 0 }, .extent = { 256, 128, 1 } },
                              .heap = kit.heap,
                              .heapOffset = 5 * kTile };
    const std::vector<metal4::MappingGroup> groups = groupsOf(kit.device, std::array{ region });
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].texture == nativeOf(kit.device, kit.texture));
    CHECK(groups[0].buffer == nullptr);
    CHECK(groups[0].heap == nativeOf(kit.device, kit.heap));
    CHECK_FALSE(groups[0].unmap);
    REQUIRE(groups[0].textureOperations.size() == 2);
    checkOperation(groups[0].textureOperations[0], MTL::SparseTextureMappingModeMap, 0, 0, { 1, 2, 0 }, { 2, 1, 1 }, 5);
    checkOperation(groups[0].textureOperations[1], MTL::SparseTextureMappingModeMap, 0, 1, { 1, 2, 0 }, { 2, 1, 1 }, 7);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a region reaching the edge of a level rounds its tiles up, and a 3D region counts slices in z", "[metal4]")
{
    Device device = *createDevice();
    Queue queue = device.queue(QueueType::Graphics);
    const Heap heap = *device.createHeap(HeapDesc{ .size = 64 * kTile, .memory = Memory::Gpu, .label = "sparse.heap" });
    const Texture odd = sparseTexture(device, TextureType::Tex2D, { 300, 200, 1 }, 1, 1, "sparse.odd");
    const Texture volume = sparseTexture(device, TextureType::Tex3D, { 256, 128, 8 }, 1, 1, "sparse.volume");
    const TileShape oddShape = device.tileShape(odd);
    const TileShape volumeShape = device.tileShape(volume);
    REQUIRE(oddShape.width == 128);
    REQUIRE(oddShape.height == 128);
    REQUIRE(volumeShape.depth == 1);
    const std::array mappings = {
        TileMapping{ .texture = odd, .region = { .offset = { 128, 0, 0 }, .extent = { 172, 200, 1 } }, .heap = heap, .heapOffset = 0 },
        TileMapping{ .texture = volume, .region = { .offset = { 128, 0, 2 }, .extent = { 128, 128, 3 } }, .heap = heap, .heapOffset = 10 * kTile },
    };
    const std::vector<metal4::MappingGroup> groups = groupsOf(device, mappings);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].textureOperations.size() == 1);
    checkOperation(groups[0].textureOperations[0], MTL::SparseTextureMappingModeMap, 0, 0, { 1, 0, 0 }, { 2, 2, 1 }, 0);
    REQUIRE(groups[1].textureOperations.size() == 1);
    checkOperation(groups[1].textureOperations[0], MTL::SparseTextureMappingModeMap, 0, 0, { 1, 0, 2 }, { 1, 1, 3 }, 10);
    // Metal's validation layer accepts both operations.
    queue.mapTiles(mappings);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a mip tail maps per layer at the first tail level, each layer's tail after the previous one", "[metal4]")
{
    Device device = *createDevice();
    const Heap heap = *device.createHeap(HeapDesc{ .size = 128 * kTile, .memory = Memory::Gpu, .label = "sparse.heap" });
    SECTION("a 2D array tail of one tile")
    {
        const Texture texture = sparseTexture(device, TextureType::Tex2DArray, { 512, 512, 1 }, 10, 2, "sparse.array");
        const TileShape shape = device.tileShape(texture);
        REQUIRE(shape.mipTailBytes == kTile);
        const TileMapping tail{ .texture = texture, .region = { .layer = 0, .layers = 2 }, .mipTail = true, .heap = heap, .heapOffset = 3 * kTile };
        const std::vector<metal4::MappingGroup> groups = groupsOf(device, std::array{ tail });
        REQUIRE(groups.size() == 1);
        REQUIRE(groups[0].textureOperations.size() == 2);
        checkOperation(groups[0].textureOperations[0], MTL::SparseTextureMappingModeMap, shape.mipTailFirst, 0, { 0, 0, 0 }, { 1, 1, 1 }, 3);
        checkOperation(groups[0].textureOperations[1], MTL::SparseTextureMappingModeMap, shape.mipTailFirst, 1, { 0, 0, 0 }, { 1, 1, 1 }, 4);
        device.queue(QueueType::Graphics).mapTiles(std::array{ tail });
    }
    SECTION("a 3D tail of many tiles")
    {
        const Texture texture = sparseTexture(device, TextureType::Tex3D, { 128, 128, 64 }, 4, 1, "sparse.volume");
        const TileShape shape = device.tileShape(texture);
        const uint32_t tiles = static_cast<uint32_t>(shape.mipTailBytes / kTile);
        REQUIRE(tiles > 1);
        const TileMapping tail{ .texture = texture, .mipTail = true, .heap = heap, .heapOffset = 2 * kTile };
        const std::vector<metal4::MappingGroup> groups = groupsOf(device, std::array{ tail });
        REQUIRE(groups.size() == 1);
        REQUIRE(groups[0].textureOperations.size() == 1);
        checkOperation(groups[0].textureOperations[0], MTL::SparseTextureMappingModeMap, shape.mipTailFirst, 0, { 0, 0, 0 }, { tiles, 1, 1 }, 2);
        device.queue(QueueType::Graphics).mapTiles(std::array{ tail });
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a buffer range maps in tiles, and an unmap names no heap", "[metal4]")
{
    SparseKit kit;
    const TileMapping range = kit.bufferTiles(2, 9 * kTile);
    TileMapping shifted = range;
    shifted.bufferOffset = 2 * kTile;
    shifted.bufferSize = 2 * kTile;
    const std::array mappings = { SparseKit::removal(shifted), range };
    const std::vector<metal4::MappingGroup> groups = groupsOf(kit.device, mappings);
    REQUIRE(groups.size() == 2);
    const MTL::Buffer* buffer = nativeOf(kit.device, kit.buffer);
    CHECK(groups[0].buffer == buffer);
    CHECK(groups[0].texture == nullptr);
    CHECK(groups[0].heap == nullptr);
    CHECK(groups[0].unmap);
    REQUIRE(groups[0].bufferOperations.size() == 1);
    CHECK(groups[0].bufferOperations[0].mode == MTL::SparseTextureMappingModeUnmap);
    CHECK(groups[0].bufferOperations[0].bufferRange.location == 2);
    CHECK(groups[0].bufferOperations[0].bufferRange.length == 2);
    CHECK(groups[0].bufferOperations[0].heapOffset == 0);
    CHECK(groups[1].buffer == buffer);
    CHECK(groups[1].heap == nativeOf(kit.device, kit.heap));
    CHECK_FALSE(groups[1].unmap);
    REQUIRE(groups[1].bufferOperations.size() == 1);
    CHECK(groups[1].bufferOperations[0].mode == MTL::SparseTextureMappingModeMap);
    CHECK(groups[1].bufferOperations[0].bufferRange.location == 0);
    CHECK(groups[1].bufferOperations[0].bufferRange.length == 2);
    CHECK(groups[1].bufferOperations[0].heapOffset == 9);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("elements group by resource, heap and mode, texture groups first, each in order of first appearance", "[metal4]")
{
    SparseKit kit;
    const Heap other = *kit.device.createHeap(HeapDesc{ .size = 4 * kTile, .memory = Memory::Gpu, .label = "sparse.other" });
    TileMapping second = kit.firstTile();
    second.region.offset.x = kit.shape.width;
    second.heap = other;
    TileMapping third = kit.firstTile();
    third.region.offset.y = kit.shape.height;
    third.heapOffset = 3 * kTile;
    TileMapping unmap = kit.firstTile();
    unmap.region.offset = { kit.shape.width, kit.shape.height, 0 };
    unmap = SparseKit::removal(unmap);
    const std::array mappings = { kit.bufferTiles(1, 10 * kTile), kit.firstTile(), second, unmap, third };
    const std::vector<metal4::MappingGroup> groups = groupsOf(kit.device, mappings);
    REQUIRE(groups.size() == 4);
    const MTL::Texture* texture = nativeOf(kit.device, kit.texture);
    CHECK(groups[0].texture == texture);
    CHECK(groups[0].heap == nativeOf(kit.device, kit.heap));
    REQUIRE(groups[0].textureOperations.size() == 2);
    checkOperation(groups[0].textureOperations[0], MTL::SparseTextureMappingModeMap, 0, 0, { 0, 0, 0 }, { 1, 1, 1 }, 0);
    checkOperation(groups[0].textureOperations[1], MTL::SparseTextureMappingModeMap, 0, 0, { 0, 1, 0 }, { 1, 1, 1 }, 3);
    CHECK(groups[1].texture == texture);
    CHECK(groups[1].heap == nativeOf(kit.device, other));
    REQUIRE(groups[1].textureOperations.size() == 1);
    checkOperation(groups[1].textureOperations[0], MTL::SparseTextureMappingModeMap, 0, 0, { 1, 0, 0 }, { 1, 1, 1 }, 0);
    CHECK(groups[2].texture == texture);
    CHECK(groups[2].heap == nullptr);
    CHECK(groups[2].unmap);
    REQUIRE(groups[2].textureOperations.size() == 1);
    checkOperation(groups[2].textureOperations[0], MTL::SparseTextureMappingModeUnmap, 0, 0, { 1, 1, 0 }, { 1, 1, 1 }, 0);
    CHECK(groups[3].buffer == nativeOf(kit.device, kit.buffer));
    CHECK(groups[3].texture == nullptr);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("mapTiles counts each native update and the signal after a buffer update, and nothing for an empty span", "[metal4]")
{
    SparseKit kit;
    const Heap other = *kit.device.createHeap(HeapDesc{ .size = 4 * kTile, .memory = Memory::Gpu, .label = "sparse.other" });
    const Counters start = kit.device.counters();
    kit.queue.mapTiles({});
    CHECK(kit.device.counters().nativeSubmitCalls == start.nativeSubmitCalls);
    // Two tiles of one texture from one heap: one update.
    TileMapping second = kit.firstTile();
    second.region.offset.x = kit.shape.width;
    second.heapOffset = kTile;
    kit.queue.mapTiles(std::array{ kit.firstTile(), second });
    CHECK(kit.device.counters().nativeSubmitCalls == start.nativeSubmitCalls + 1);
    // A buffer, a texture from another heap and a texture unmap: three updates and the signal that ends a buffer update.
    TileMapping third = kit.firstTile();
    third.region.offset.y = kit.shape.height;
    third.heap = other;
    third.heapOffset = 0;
    kit.queue.mapTiles(std::array{ kit.bufferTiles(1, 10 * kTile), third, SparseKit::removal(kit.firstTile()) });
    CHECK(kit.device.counters().nativeSubmitCalls == start.nativeSubmitCalls + 5);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a texture mapping that follows a buffer mapping on the same queue runs", "[metal4]")
{
    SparseKit kit;
    kit.queue.mapTiles(std::array{ kit.firstTile() });
    kit.queue.mapTiles(std::array{ kit.bufferTiles(1, 10 * kTile) });
    TileMapping next = kit.firstTile();
    next.region.offset.x = kit.shape.width;
    next.heapOffset = kTile;
    kit.queue.mapTiles(std::array{ next });
    kit.queue.mapTiles(std::array{ kit.bufferTiles(1, 11 * kTile), SparseKit::removal(next) });
    kit.queue.mapTiles(std::array{ SparseKit::removal(kit.firstTile()) });
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("texture and buffer mappings that alternate across queues run", "[metal4]")
{
    SparseKit kit;
    const Heap other = *kit.device.createHeap(HeapDesc{ .size = 4 * kTile, .memory = Memory::Gpu, .label = "sparse.other" });
    Queue graphics = kit.device.queue(QueueType::Graphics);
    Queue compute = kit.device.queue(QueueType::Compute);
    graphics.mapTiles(std::array{ kit.firstTile() });
    compute.mapTiles(std::array{ kit.bufferTiles(1, 10 * kTile) });
    TileMapping next = kit.firstTile();
    next.region.offset.x = kit.shape.width;
    next.heap = other;
    graphics.mapTiles(std::array{ next });
    TileMapping last = kit.firstTile();
    last.region.offset.y = kit.shape.height;
    last.heapOffset = kTile;
    compute.mapTiles(std::array{ last });
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("mapping, remapping, unmapping and destroying mapped resources run clean on every queue type", "[metal4]")
{
    const QueueType type = GENERATE(QueueType::Graphics, QueueType::Compute, QueueType::Copy);
    CAPTURE(static_cast<int>(type));
    SparseKit kit;
    Queue queue = kit.device.queue(type);
    queue.mapTiles(std::array{ kit.wholeLevel(0), kit.tail(16 * kTile), kit.bufferTiles(SparseKit::kBufferTiles, 20 * kTile) });
    TileMapping again = kit.firstTile();
    again.heapOffset = 30 * kTile;
    queue.mapTiles(std::array{ again, kit.bufferTiles(1, 31 * kTile) });
    queue.mapTiles(std::array{ SparseKit::removal(again), SparseKit::removal(kit.tail()), SparseKit::removal(kit.bufferTiles()) });
    kit.device.destroy(kit.texture);
    kit.device.destroy(kit.buffer);
    kit.device.destroy(kit.heap);
    // The default three frames in flight and one more retire the destroyed objects before the device goes.
    runFrames(kit.device, 4);
    destroyDevice(kit.device);
}
