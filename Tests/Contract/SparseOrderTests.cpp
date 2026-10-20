//----------------------------------------------------------------------------------------------------------------------
/// @file SparseOrderTests.cpp
/// @brief Tests that the tracker accepts mapping changes fences, the CPU or queue order order, and ordered first uses of new tiles.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// The bytes the buffer tests copy into the sparse buffer.
    [[maybe_unused]] constexpr uint64_t kBytes = 256;

    /// A `Memory::Gpu` heap, a sparse buffer of four tiles and a sparse 2D texture of two layers with a mip tail.
    struct SparseSet
    {
        Heap heap;       ///< The heap tiles map onto.
        Buffer buffer;   ///< The sparse buffer.
        Texture texture; ///< The sparse texture.
        TileShape shape; ///< The tile shape of `texture`.
    };

    //==================================================================================================================
    /// Creates the sparse resources of a test on the device of `kit`.
    [[maybe_unused]] SparseSet sparse(PassKit& kit)
    {
        SparseSet set;
        set.heap = *kit.device.createHeap(HeapDesc{ .size = 64 * 65536, .memory = Memory::Gpu, .label = "test.sparse.heap" });
        set.buffer =
            *kit.device.createBuffer(BufferDesc{ .size = 4 * 65536, .memory = Memory::Gpu, .usage = BufferUsage::Sparse, .label = "test.sparse.buffer" });
        set.texture = *kit.device.createTexture(
            TextureDesc{ .type = TextureType::Tex2DArray,
                         .format = Format::RGBA8Unorm,
                         .extent = { 512, 512, 1 },
                         .mips = 10,
                         .layers = 2,
                         .usage = TextureUsage::Sampled | TextureUsage::Sparse,
                         .label = "test.sparse.texture" }
        );
        set.shape = kit.device.tileShape(set.texture);
        return set;
    }

    //==================================================================================================================
    /// Returns the mapping of the first tile of `set`'s buffer onto the heap's first tile.
    [[maybe_unused]] TileMapping bufferTile(const SparseSet& set)
    {
        return TileMapping{ .buffer = set.buffer, .bufferSize = 65536, .heap = set.heap };
    }

    //==================================================================================================================
    /// Returns the region of tile `x` of row 0 of mip 0, layer 0 of `set`'s texture.
    [[maybe_unused]] TextureRegion tileRegion(const SparseSet& set, uint32_t x)
    {
        return TextureRegion{ .offset = { x * set.shape.width, 0, 0 }, .extent = { set.shape.width, set.shape.height, 1 } };
    }

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`, waiting on `wait` and signalling `signal`.
    [[maybe_unused]] void submitTo(
        Device device, CommandList list, QueueType type, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {}
    )
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, wait, signal);
    }

    //==================================================================================================================
    /// Submits a compute list that moves every subresource of `set`'s texture from `Undefined` to `CopyDst`, signalling
    /// `moved`, and an empty graphics submission that waits on it, so the move is ordered before what Graphics does next.
    [[maybe_unused]] void moveOnCompute(PassKit& kit, const SparseSet& set, std::span<const FenceValue> moved)
    {
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.move");
        test::transition(compute, set.texture, Layout::Undefined, Layout::CopyDst);
        submitTo(kit.device, compute, QueueType::Compute, {}, moved);
        kit.device.queue(QueueType::Graphics).submit({}, moved);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a sparse buffer copied to on Compute is remapped on Graphics after a fence wait", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    const Buffer source = kit.buffer(BufferUsage::None, kBytes);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> copied{ FenceValue{ fence, 1 } };
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBuffer(source, 0, set.buffer, 0, kBytes);
    submitTo(kit.device, compute, QueueType::Compute, {}, copied);
    // The mapping is ordered after the copy by an earlier submission's wait on its queue.
    submitTo(kit.device, kit.list, QueueType::Graphics, copied);
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ bufferTile(set) });
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a sparse buffer remapped on Graphics is copied to on Compute after a fence wait", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    const Buffer source = kit.buffer(BufferUsage::None, kBytes);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> mapped{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> copied{ FenceValue{ fence, 2 } };
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ bufferTile(set) });
    kit.device.queue(QueueType::Graphics).submit({}, {}, mapped);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.barrier(std::array{ GlobalBarrier{ Stage::Copy, Access::None, Stage::Copy, Access::CopyWrite } });
    compute.copyBuffer(source, 0, set.buffer, 0, kBytes);
    submitTo(kit.device, compute, QueueType::Compute, mapped, copied);
    submitTo(kit.device, kit.list, QueueType::Graphics, copied);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a sparse buffer copied to on Compute is remapped on Graphics after the CPU waits for the copy", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    const Buffer source = kit.buffer(BufferUsage::None, kBytes);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> copied{ FenceValue{ fence, 1 } };
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBuffer(source, 0, set.buffer, 0, kBytes);
    submitTo(kit.device, compute, QueueType::Compute, {}, copied);
    REQUIRE(kit.device.wait(copied[0], UINT64_MAX));
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ bufferTile(set) });
    // The frame still joins the compute work into its graphics work.
    submitTo(kit.device, kit.list, QueueType::Graphics, copied);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a copy on Compute to other tiles of a texture than Graphics remaps after it needs no fence", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    const Buffer source = kit.buffer(BufferUsage::None, 65536);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> copied{ FenceValue{ fence, 2 } };
    moveOnCompute(kit, set, moved);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, tileRegion(set, 0));
    submitTo(kit.device, compute, QueueType::Compute, {}, copied);
    const TileMapping other{ .texture = set.texture, .region = tileRegion(set, 1), .heap = set.heap };
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ other });
    submitTo(kit.device, kit.list, QueueType::Graphics, copied);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a copy on Compute to other tiles of a texture than Graphics remapped before it needs no fence", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    const Buffer source = kit.buffer(BufferUsage::None, 65536);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> copied{ FenceValue{ fence, 2 } };
    moveOnCompute(kit, set, moved);
    const TileMapping other{ .texture = set.texture, .region = tileRegion(set, 1), .heap = set.heap };
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ other });
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, tileRegion(set, 0));
    submitTo(kit.device, compute, QueueType::Compute, {}, copied);
    submitTo(kit.device, kit.list, QueueType::Graphics, copied);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a copy on Compute to the mip tail of one layer needs no fence against Graphics remapping another layer's tail", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    REQUIRE(set.shape.mipTailFirst < 10);
    const Buffer source = kit.buffer(BufferUsage::None, kBytes);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> copied{ FenceValue{ fence, 2 } };
    moveOnCompute(kit, set, moved);
    const TileMapping tail{ .texture = set.texture, .region = { .mip = set.shape.mipTailFirst, .layer = 0 }, .mipTail = true, .heap = set.heap };
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ tail });
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    const TextureRegion texel{ .mip = 9, .layer = 1, .extent = { 1, 1, 1 } };
    compute.copyBufferToTexture(source, { .rowPitch = 256 }, set.texture, texel);
    submitTo(kit.device, compute, QueueType::Compute, {}, copied);
    submitTo(kit.device, kit.list, QueueType::Graphics, copied);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a sparse buffer remapped and copied to on one queue is ordered by queue order alone", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    const Buffer source = kit.buffer(BufferUsage::None, kBytes);
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ bufferTile(set) });
    kit.list.barrier(std::array{ GlobalBarrier{ Stage::Copy, Access::None, Stage::Copy, Access::CopyWrite } });
    kit.list.copyBuffer(source, 0, set.buffer, 0, kBytes);
    submitTo(kit.device, kit.list, QueueType::Graphics);
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ TileMapping{ .buffer = set.buffer, .bufferSize = 65536, .unmap = true } });
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a newly mapped texture tile discarded from Undefined is copied to", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    const Buffer source = kit.buffer(BufferUsage::None, 65536);
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ TileMapping{ .texture = set.texture, .region = tileRegion(set, 0), .heap = set.heap } });
    test::transition(kit.list, set.texture, Layout::Undefined, Layout::CopyDst);
    kit.list.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, tileRegion(set, 0));
    submitTo(kit.device, kit.list, QueueType::Graphics);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a newly mapped buffer tile is copied to after a global barrier", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    const Buffer source = kit.buffer(BufferUsage::None, kBytes);
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ bufferTile(set) });
    kit.list.barrier(std::array{ GlobalBarrier{ Stage::Copy, Access::None, Stage::Copy, Access::CopyWrite } });
    kit.list.copyBuffer(source, 0, set.buffer, 0, kBytes);
    submitTo(kit.device, kit.list, QueueType::Graphics);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a copy to an old tile of a subresource whose other tile was newly mapped needs no discard", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    const Buffer source = kit.buffer(BufferUsage::None, 65536);
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ TileMapping{ .texture = set.texture, .region = tileRegion(set, 0), .heap = set.heap } });
    test::transition(kit.list, set.texture, Layout::Undefined, Layout::CopyDst);
    kit.list.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, tileRegion(set, 0));
    submitTo(kit.device, kit.list, QueueType::Graphics);
    // Only tile 1 is new; the copy's box misses it, so tile 0 keeps its contents and needs no discard.
    const TileMapping other{ .texture = set.texture, .region = tileRegion(set, 1), .heap = set.heap, .heapOffset = 65536 };
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ other });
    CommandList graphics = kit.device.acquireCommandList(QueueType::Graphics, "test.graphics");
    graphics.barrier(std::array{ GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyWrite } });
    graphics.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, tileRegion(set, 0));
    submitTo(kit.device, graphics, QueueType::Graphics);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("an unmapped tile leaves no first use to order", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    test::transition(kit.list, set.texture, Layout::Undefined, Layout::General);
    submitTo(kit.device, kit.list, QueueType::Graphics);
    const TileMapping tile{ .texture = set.texture, .region = tileRegion(set, 0), .heap = set.heap };
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ tile });
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ TileMapping{ .texture = set.texture, .region = tileRegion(set, 0), .unmap = true } });
    // No tile of the texture is newly mapped any more, so a barrier that keeps the contents is no first use.
    CommandList graphics = kit.device.acquireCommandList(QueueType::Graphics, "test.graphics");
    test::transition(graphics, set.texture, Layout::General, Layout::CopySrc);
    submitTo(kit.device, graphics, QueueType::Graphics);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a mapping on Compute is first used on Graphics after a fence and a global barrier there", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const SparseSet set = sparse(kit);
    const Buffer source = kit.buffer(BufferUsage::None, kBytes);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> mapped{ FenceValue{ fence, 1 } };
    kit.device.queue(QueueType::Compute).mapTiles(std::array{ bufferTile(set) });
    kit.device.queue(QueueType::Compute).submit({}, {}, mapped);
    kit.list.barrier(std::array{ GlobalBarrier{ Stage::Copy, Access::None, Stage::Copy, Access::CopyWrite } });
    kit.list.copyBuffer(source, 0, set.buffer, 0, kBytes);
    submitTo(kit.device, kit.list, QueueType::Graphics, mapped);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}
