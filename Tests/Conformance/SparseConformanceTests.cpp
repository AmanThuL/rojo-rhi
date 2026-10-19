//----------------------------------------------------------------------------------------------------------------------
/// @file SparseConformanceTests.cpp
/// @brief Observes on a GPU that tiles mapped by `Queue::mapTiles` hold what copies write and what other mappings read.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace rojoRHI;

namespace {
    /// Bytes of one tile of a buffer or heap on every backend that takes the placement path.
    constexpr uint64_t kTile = 65536;
    /// Tiles each test heap holds, enough for every mapping a case makes.
    constexpr uint64_t kHeapTiles = 8;
    /// Edge of the sparse textures, in texels of `RGBA8Unorm`.
    constexpr uint32_t kEdge = 512;

    //==================================================================================================================
    /// Skips the running case when the device has no placement sparse resources.
    void requirePlacementSparse(const test::GpuKit& kit)
    {
        if (!kit.device.capabilities().placementSparse)
        {
            SKIP("the device reports no placement sparse resources");
        }
    }

    //==================================================================================================================
    /// Returns a `Memory::Gpu` heap of `kHeapTiles` tiles.
    Heap makeHeap(test::GpuKit& kit)
    {
        return *kit.device.createHeap(HeapDesc{ .size = kHeapTiles * kTile, .memory = Memory::Gpu, .label = "test.sparse.heap" });
    }

    //==================================================================================================================
    /// Returns a sparse buffer of `tiles` tiles.
    Buffer makeSparseBuffer(test::GpuKit& kit, uint64_t tiles)
    {
        return *kit.device.createBuffer(
            BufferDesc{ .size = tiles * kTile, .memory = Memory::Gpu, .usage = BufferUsage::Sparse, .label = "test.sparse.buffer" }
        );
    }

    //==================================================================================================================
    /// Returns a sparse `RGBA8Unorm` 2D array texture of `kEdge` square texels, a full mip chain and two layers.
    Texture makeSparseTexture(test::GpuKit& kit)
    {
        return *kit.device.createTexture(
            TextureDesc{ .type = TextureType::Tex2DArray,
                         .format = Format::RGBA8Unorm,
                         .extent = { kEdge, kEdge, 1 },
                         .mips = 10,
                         .layers = 2,
                         .usage = TextureUsage::Sampled | TextureUsage::Sparse,
                         .label = "test.sparse.texture" }
        );
    }

    //==================================================================================================================
    /// The access pair of a copy write followed by a copy read.
    GlobalBarrier copyWriteToRead()
    {
        return GlobalBarrier{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead };
    }

    //==================================================================================================================
    /// Returns the `rowPitch` of a row of `width` texels of 4 bytes, padded to 256 bytes.
    uint32_t pitchOf(uint32_t width)
    {
        return (width * 4 + 255) / 256 * 256;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a mapped sparse buffer tile holds what a copy wrote", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    requirePlacementSparse(kit);
    const Heap heap = makeHeap(kit);
    const Buffer sparse = makeSparseBuffer(kit, 2);
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ TileMapping{ .buffer = sparse, .bufferSize = 2 * kTile, .heap = heap } });
    const std::vector<std::byte> bytes = test::pattern(2 * kTile, 5);
    const Buffer upload = kit.buffer(2 * kTile, Memory::Upload, BufferUsage::None, bytes);
    const Buffer readback = kit.buffer(2 * kTile, Memory::Readback);
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    list.copyBuffer(upload, 0, sparse, 0, 2 * kTile);
    list.barrier(std::array{ copyWriteToRead() });
    list.copyBuffer(sparse, 0, readback, 0, 2 * kTile);
    kit.run(std::array{ list });
    CHECK(kit.read(readback) == bytes);
}

//======================================================================================================================
TEST_CASE("a heap tile mapped into a second sparse buffer reads the first one's data", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    requirePlacementSparse(kit);
    const Heap heap = makeHeap(kit);
    const Buffer first = makeSparseBuffer(kit, 1);
    const Buffer second = makeSparseBuffer(kit, 1);
    Queue graphics = kit.device.queue(QueueType::Graphics);
    graphics.mapTiles(std::array{ TileMapping{ .buffer = first, .bufferSize = kTile, .heap = heap } });
    const std::vector<std::byte> bytes = test::pattern(kTile, 9);
    const Buffer upload = kit.buffer(kTile, Memory::Upload, BufferUsage::None, bytes);
    CommandList write = kit.list(QueueType::Graphics, "test.write");
    write.copyBuffer(upload, 0, first, 0, kTile);
    kit.run(std::array{ write });
    // The second buffer's first use of memory another resource wrote follows the aliasing rule: a global barrier.
    graphics.mapTiles(std::array{ TileMapping{ .buffer = second, .bufferSize = kTile, .heap = heap } });
    const Buffer readback = kit.buffer(kTile, Memory::Readback);
    CommandList read = kit.list(QueueType::Graphics, "test.read");
    read.barrier(std::array{ copyWriteToRead() });
    read.copyBuffer(second, 0, readback, 0, kTile);
    kit.run(std::array{ read });
    CHECK(kit.read(readback) == bytes);
}

//======================================================================================================================
TEST_CASE("a mapped texture region holds what copyBufferToTexture wrote", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    requirePlacementSparse(kit);
    const Heap heap = makeHeap(kit);
    const Texture texture = makeSparseTexture(kit);
    const TileShape shape = kit.device.tileShape(texture);
    const TextureRegion tile{ .layer = 1, .extent = { shape.width, shape.height, 1 } };
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ TileMapping{ .texture = texture, .region = tile, .heap = heap } });
    const uint32_t pitch = pitchOf(shape.width);
    const std::vector<std::byte> rows = test::pattern(uint64_t{ pitch } * shape.height, 13);
    const Buffer upload = kit.buffer(rows.size(), Memory::Upload, BufferUsage::None, rows);
    const BufferTextureLayout layout{ .offset = 0, .rowPitch = pitch, .rowsPerImage = shape.height };
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    test::transition(list, texture, Layout::Undefined, Layout::General);
    list.copyBufferToTexture(upload, layout, texture, tile);
    kit.run(std::array{ list });
    const std::vector<std::byte> got = kit.readTexture(texture, tile, Format::RGBA8Unorm);
    bool equal = got.size() == uint64_t{ shape.width } * 4 * shape.height;
    for (uint32_t row = 0; equal && row < shape.height; ++row)
    {
        for (uint32_t byte = 0; equal && byte < shape.width * 4; ++byte)
        {
            equal = got[uint64_t{ row } * shape.width * 4 + byte] == rows[uint64_t{ row } * pitch + byte];
        }
    }
    CHECK(equal);
}

//======================================================================================================================
TEST_CASE("a mip tail maps and holds data", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    requirePlacementSparse(kit);
    const Heap heap = makeHeap(kit);
    const Texture texture = makeSparseTexture(kit);
    const TileShape shape = kit.device.tileShape(texture);
    if (shape.mipTailFirst == 0 || shape.mipTailFirst >= 10)
    {
        SKIP("the texture has no mip tail above mip 0");
    }
    kit.device.queue(QueueType::Graphics)
        .mapTiles(std::array{ TileMapping{ .texture = texture, .region = { .mip = shape.mipTailFirst }, .mipTail = true, .heap = heap } });
    // The first tail level, and mip 9 of one texel, both lie in the tail.
    const uint32_t firstEdge = kEdge >> shape.mipTailFirst;
    const TextureRegion firstLevel{ .mip = shape.mipTailFirst, .extent = { firstEdge, firstEdge, 1 } };
    const TextureRegion lastLevel{ .mip = 9, .extent = { 1, 1, 1 } };
    const uint32_t firstPitch = pitchOf(firstEdge);
    const std::vector<std::byte> firstRows = test::pattern(uint64_t{ firstPitch } * firstEdge, 17);
    const std::vector<std::byte> lastRows = test::pattern(256, 19);
    const Buffer firstUpload = kit.buffer(firstRows.size(), Memory::Upload, BufferUsage::None, firstRows);
    const Buffer lastUpload = kit.buffer(lastRows.size(), Memory::Upload, BufferUsage::None, lastRows);
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    test::transition(list, texture, Layout::Undefined, Layout::General);
    list.copyBufferToTexture(firstUpload, BufferTextureLayout{ .rowPitch = firstPitch, .rowsPerImage = firstEdge }, texture, firstLevel);
    list.copyBufferToTexture(lastUpload, BufferTextureLayout{ .rowPitch = 256, .rowsPerImage = 1 }, texture, lastLevel);
    kit.run(std::array{ list });
    const std::vector<std::byte> firstGot = kit.readTexture(texture, firstLevel, Format::RGBA8Unorm);
    bool equal = firstGot.size() == uint64_t{ firstEdge } * 4 * firstEdge;
    for (uint32_t row = 0; equal && row < firstEdge; ++row)
    {
        for (uint32_t byte = 0; equal && byte < firstEdge * 4; ++byte)
        {
            equal = firstGot[uint64_t{ row } * firstEdge * 4 + byte] == firstRows[uint64_t{ row } * firstPitch + byte];
        }
    }
    CHECK(equal);
    const std::vector<std::byte> lastGot = kit.readTexture(texture, lastLevel, Format::RGBA8Unorm);
    REQUIRE(lastGot.size() == 4);
    CHECK(std::equal(lastGot.begin(), lastGot.end(), lastRows.begin()));
}

//======================================================================================================================
TEST_CASE("a mapping on the compute queue, ordered by a fence, is seen by a graphics copy", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    requirePlacementSparse(kit);
    const Heap heap = makeHeap(kit);
    const Buffer sparse = makeSparseBuffer(kit, 1);
    const Fence mapped = *kit.device.createFence(0, "test.mapped");
    Queue compute = kit.device.queue(QueueType::Compute);
    compute.mapTiles(std::array{ TileMapping{ .buffer = sparse, .bufferSize = kTile, .heap = heap } });
    // An empty submission after the mapping signals once the mapping has executed.
    compute.submit({}, {}, std::array{ FenceValue{ mapped, 1 } });
    const std::vector<std::byte> bytes = test::pattern(kTile, 23);
    const Buffer upload = kit.buffer(kTile, Memory::Upload, BufferUsage::None, bytes);
    const Buffer readback = kit.buffer(kTile, Memory::Readback);
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    list.copyBuffer(upload, 0, sparse, 0, kTile);
    list.barrier(std::array{ copyWriteToRead() });
    list.copyBuffer(sparse, 0, readback, 0, kTile);
    kit.run(std::array{ list }, std::array{ FenceValue{ mapped, 1 } });
    CHECK(kit.read(readback) == bytes);
}

//======================================================================================================================
TEST_CASE("a mapping applies after the work submitted before it on its queue", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    requirePlacementSparse(kit);
    const Heap heap = makeHeap(kit);
    const Texture texture = makeSparseTexture(kit);
    const Buffer sparse = makeSparseBuffer(kit, 1);
    const TileShape shape = kit.device.tileShape(texture);
    const TextureRegion tile{ .extent = { shape.width, shape.height, 1 } };
    const uint32_t pitch = pitchOf(shape.width);
    const BufferTextureLayout layout{ .offset = 0, .rowPitch = pitch, .rowsPerImage = shape.height };
    Queue graphics = kit.device.queue(QueueType::Graphics);
    // The texture tile and the buffer tile start on heap tiles 0 and 1, each written with its own pattern; tiles 2 and 3 hold
    // other patterns.
    graphics.mapTiles(
        std::array{ TileMapping{ .texture = texture, .region = tile, .heap = heap },
                    TileMapping{ .buffer = sparse, .bufferSize = kTile, .heap = heap, .heapOffset = kTile } }
    );
    const Buffer others = makeSparseBuffer(kit, 2);
    graphics.mapTiles(std::array{ TileMapping{ .buffer = others, .bufferSize = 2 * kTile, .heap = heap, .heapOffset = 2 * kTile } });
    const std::vector<std::byte> texels = test::pattern(uint64_t{ pitch } * shape.height, 29);
    const std::vector<std::byte> bytes = test::pattern(kTile, 31);
    const std::vector<std::byte> other = test::pattern(2 * kTile, 37);
    const Buffer texelUpload = kit.buffer(texels.size(), Memory::Upload, BufferUsage::None, texels);
    const Buffer byteUpload = kit.buffer(kTile, Memory::Upload, BufferUsage::None, bytes);
    const Buffer otherUpload = kit.buffer(2 * kTile, Memory::Upload, BufferUsage::None, other);
    CommandList write = kit.list(QueueType::Graphics, "test.write");
    test::transition(write, texture, Layout::Undefined, Layout::General);
    write.barrier(std::array{ GlobalBarrier{ .srcStage = Stage::Copy, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite } });
    write.copyBufferToTexture(texelUpload, layout, texture, tile);
    write.copyBuffer(byteUpload, 0, sparse, 0, kTile);
    write.copyBuffer(otherUpload, 0, others, 0, 2 * kTile);
    kit.run(std::array{ write });
    // A long copy keeps the reads of the two tiles behind it; the remapping submitted after them must wait for them.
    constexpr uint64_t kLong = uint64_t{ 64 } << 20;
    const Buffer from = kit.buffer(kLong, Memory::Gpu);
    const Buffer to = kit.buffer(kLong, Memory::Gpu);
    const Buffer texelReadback = kit.buffer(texels.size(), Memory::Readback);
    const Buffer byteReadback = kit.buffer(kTile, Memory::Readback);
    CommandList read = kit.list(QueueType::Graphics, "test.read");
    read.copyBuffer(from, 0, to, 0, kLong);
    read.barrier(std::array{ copyWriteToRead() });
    read.copyTextureToBuffer(texture, tile, texelReadback, layout);
    read.copyBuffer(sparse, 0, byteReadback, 0, kTile);
    read.end();
    graphics.submit(std::array{ read });
    graphics.mapTiles(
        std::array{ TileMapping{ .texture = texture, .region = tile, .heap = heap, .heapOffset = 2 * kTile },
                    TileMapping{ .buffer = sparse, .bufferSize = kTile, .heap = heap, .heapOffset = 3 * kTile } }
    );
    // An empty list run after the remapping waits for everything submitted before it.
    kit.run(std::array{ kit.list(QueueType::Graphics, "test.join") });
    const std::vector<std::byte> texelsGot = kit.read(texelReadback);
    bool equal = true;
    for (uint32_t row = 0; equal && row < shape.height; ++row)
    {
        for (uint32_t byte = 0; equal && byte < shape.width * 4; ++byte)
        {
            equal = texelsGot[uint64_t{ row } * pitch + byte] == texels[uint64_t{ row } * pitch + byte];
        }
    }
    CHECK(equal);
    const bool sameBytes = kit.read(byteReadback) == bytes;
    CHECK(sameBytes);
}

//======================================================================================================================
TEST_CASE("work submitted after a mapping on its queue writes the mapped tiles", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    requirePlacementSparse(kit);
    const Heap heap = makeHeap(kit);
    Queue graphics = kit.device.queue(QueueType::Graphics);
    // A copy that ran ahead of its mapping would leave the heap tile with what an earlier round wrote there, so each round
    // writes its own pattern to a fresh texture, a region tile or a mip tail in turn, on the heap tiles in rotation. One
    // round rarely catches a copy ahead of its mapping, so the case runs many.
    constexpr uint32_t kRounds = 512;
    uint32_t mismatches = 0;
    for (uint32_t round = 0; round < kRounds; ++round)
    {
        const Texture texture = makeSparseTexture(kit);
        const TileShape shape = kit.device.tileShape(texture);
        const bool tail = round % 2 == 1 && shape.mipTailFirst > 0 && shape.mipTailFirst < 10;
        const uint32_t mip = tail ? shape.mipTailFirst : 0;
        const uint32_t width = tail ? kEdge >> mip : shape.width;
        const uint32_t height = tail ? kEdge >> mip : shape.height;
        const TextureRegion region{ .mip = mip, .layer = 1, .extent = { width, height, 1 } };
        const uint32_t pitch = pitchOf(width);
        const std::vector<std::byte> rows = test::pattern(uint64_t{ pitch } * height, 41 + round);
        const Buffer upload = kit.buffer(rows.size(), Memory::Upload, BufferUsage::None, rows);
        CommandList list = kit.list(QueueType::Graphics, "test.copy");
        test::transition(list, texture, Layout::Undefined, Layout::General);
        list.copyBufferToTexture(upload, BufferTextureLayout{ .rowPitch = pitch, .rowsPerImage = height }, texture, region);
        const uint64_t heapOffset = round % kHeapTiles * kTile;
        if (tail)
        {
            graphics.mapTiles(
                std::array{ TileMapping{ .texture = texture, .region = { .mip = mip, .layer = 1 }, .mipTail = true, .heap = heap, .heapOffset = heapOffset } }
            );
        }
        else
        {
            graphics.mapTiles(std::array{ TileMapping{ .texture = texture, .region = region, .heap = heap, .heapOffset = heapOffset } });
        }
        kit.run(std::array{ list });
        const std::vector<std::byte> got = kit.readTexture(texture, region, Format::RGBA8Unorm);
        bool equal = got.size() == uint64_t{ width } * 4 * height;
        for (uint32_t row = 0; equal && row < height; ++row)
        {
            for (uint32_t byte = 0; equal && byte < width * 4; ++byte)
            {
                equal = got[uint64_t{ row } * width * 4 + byte] == rows[uint64_t{ row } * pitch + byte];
            }
        }
        mismatches += equal ? 0 : 1;
        kit.device.destroy(texture);
    }
    CHECK(mismatches == 0);
}
