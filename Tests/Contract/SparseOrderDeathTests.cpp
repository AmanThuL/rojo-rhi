//----------------------------------------------------------------------------------------------------------------------
/// @file SparseOrderDeathTests.cpp
/// @brief Death tests for tile uses a mapping change leaves unordered: on another queue with no fence, or a first use.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// The bytes every test copies into the sparse buffer.
    constexpr uint64_t kBytes = 256;

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
    SparseSet sparse(PassKit& kit)
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
    TileMapping bufferTile(const SparseSet& set)
    {
        return TileMapping{ .buffer = set.buffer, .bufferSize = 65536, .heap = set.heap };
    }

    //==================================================================================================================
    /// Returns the region of the first tile of mip 0, layer 0 of `set`'s texture.
    TextureRegion firstTile(const SparseSet& set)
    {
        return TextureRegion{ .extent = { set.shape.width, set.shape.height, 1 } };
    }

    //==================================================================================================================
    /// Returns the mapping of the first tile of mip 0, layer 0 of `set`'s texture onto the heap's first tile.
    TileMapping textureTile(const SparseSet& set)
    {
        return TileMapping{ .texture = set.texture, .region = firstTile(set), .heap = set.heap };
    }

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`, waiting on `wait` and signalling `signal`.
    void submitTo(Device device, CommandList list, QueueType type, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {})
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, wait, signal);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0013 a sparse buffer copied to on Compute is remapped on Graphics with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0013", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, kBytes);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.copyBuffer(source, 0, set.buffer, 0, kBytes);
        submitTo(kit.device, compute, QueueType::Compute);
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ bufferTile(set) });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0013 a sparse buffer remapped on Graphics is copied to on Compute with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0013", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, kBytes);
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ bufferTile(set) });
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.copyBuffer(source, 0, set.buffer, 0, kBytes);
        submitTo(kit.device, compute, QueueType::Compute);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0013 a sparse texture remapped on Graphics is moved on Compute with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0013", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const TileMapping tile{ .texture = set.texture, .region = { .extent = { set.shape.width, set.shape.height, 1 } }, .heap = set.heap };
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ tile });
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        test::transition(compute, set.texture, Layout::Undefined, Layout::General);
        submitTo(kit.device, compute, QueueType::Compute);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0013 a sparse texture copied on Compute is remapped on Graphics with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0013", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, 65536);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        test::transition(compute, set.texture, Layout::Undefined, Layout::General);
        compute.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, firstTile(set));
        submitTo(kit.device, compute, QueueType::Compute);
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ textureTile(set) });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0013 a sparse buffer written and remapped on Graphics is written on Compute with no fence, not queue-0005", "[contract][death]")
{
    // The Compute copy races both the Graphics copy (queue-0005) and the remap; the mapping rule reports first.
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0013", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, kBytes);
        kit.list.copyBuffer(source, 0, set.buffer, 0, kBytes);
        submitTo(kit.device, kit.list, QueueType::Graphics);
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ bufferTile(set) });
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.copyBuffer(source, 0, set.buffer, 0, kBytes);
        submitTo(kit.device, compute, QueueType::Compute);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0013 a sparse texture written and remapped on Graphics is written on Compute with no fence, not queue-0005", "[contract][death]")
{
    // A fence hands the texture over in General before the Graphics copy, so only the copy and the remap race the Compute copy.
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0013", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, 65536);
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
        test::transition(kit.list, set.texture, Layout::Undefined, Layout::General);
        submitTo(kit.device, kit.list, QueueType::Graphics, {}, moved);
        CommandList graphics = kit.device.acquireCommandList(QueueType::Graphics, "test.graphics");
        graphics.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, firstTile(set));
        submitTo(kit.device, graphics, QueueType::Graphics);
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ textureTile(set) });
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, firstTile(set));
        submitTo(kit.device, compute, QueueType::Compute, moved);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0013 a copy on Compute targets a texture tile Graphics remapped with no fence", "[contract][death]")
{
    // The move to General is ordered before the remap, so only the copy's tile meets it.
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0013", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, 65536);
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
        CommandList move = kit.device.acquireCommandList(QueueType::Compute, "test.move");
        test::transition(move, set.texture, Layout::Undefined, Layout::General);
        submitTo(kit.device, move, QueueType::Compute, {}, moved);
        kit.device.queue(QueueType::Graphics).submit({}, moved);
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ textureTile(set) });
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, firstTile(set));
        submitTo(kit.device, compute, QueueType::Compute);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0014 a newly mapped buffer tile is copied to with no global barrier", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0014", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, kBytes);
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ bufferTile(set) });
        kit.list.copyBuffer(source, 0, set.buffer, 0, kBytes);
        submitTo(kit.device, kit.list, QueueType::Graphics);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0014 a newly mapped texture tile is copied to without a discard", "[contract][death]")
{
    // The texture holds defined contents in General before the mapping, so the move to CopyDst keeps them: no discard.
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0014", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, 65536);
        test::transition(kit.list, set.texture, Layout::Undefined, Layout::General);
        submitTo(kit.device, kit.list, QueueType::Graphics);
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ textureTile(set) });
        CommandList graphics = kit.device.acquireCommandList(QueueType::Graphics, "test.graphics");
        test::transition(graphics, set.texture, Layout::General, Layout::CopyDst);
        graphics.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, firstTile(set));
        submitTo(kit.device, graphics, QueueType::Graphics);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0014 a texture tile remapped after a discard is used without another", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0014", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, 65536);
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ textureTile(set) });
        test::transition(kit.list, set.texture, Layout::Undefined, Layout::CopyDst);
        kit.list.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, firstTile(set));
        submitTo(kit.device, kit.list, QueueType::Graphics);
        // The tile moves to other heap memory: the copy below is its first use there.
        TileMapping moved = textureTile(set);
        moved.heapOffset = 65536;
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ moved });
        CommandList graphics = kit.device.acquireCommandList(QueueType::Graphics, "test.graphics");
        // A global barrier orders the two copies (sync-0021) and discards nothing.
        graphics.barrier(std::array{ GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyWrite } });
        graphics.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, firstTile(set));
        submitTo(kit.device, graphics, QueueType::Graphics);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0014 a newly mapped buffer tile is first used on another queue, after a fence, with no global barrier", "[contract][death]")
{
    // The fence orders the mapping before the copy (no sparse-0013); only the global barrier is missing.
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0014", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, kBytes);
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> mapped{ FenceValue{ fence, 1 } };
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ bufferTile(set) });
        kit.device.queue(QueueType::Graphics).submit({}, {}, mapped);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.copyBuffer(source, 0, set.buffer, 0, kBytes);
        submitTo(kit.device, compute, QueueType::Compute, mapped);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0014 a newly mapped mip tail is moved from a defined layout and copied to", "[contract][death]")
{
    // A tail mapping marks every tail mip of its layer as a whole, so a barrier that keeps the contents reaches it.
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0014", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, kBytes);
        test::transition(kit.list, set.texture, Layout::Undefined, Layout::General);
        submitTo(kit.device, kit.list, QueueType::Graphics);
        const TileMapping tail{ .texture = set.texture, .region = { .mip = set.shape.mipTailFirst }, .mipTail = true, .heap = set.heap };
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ tail });
        CommandList graphics = kit.device.acquireCommandList(QueueType::Graphics, "test.graphics");
        test::transition(graphics, set.texture, Layout::General, Layout::CopyDst);
        graphics.copyBufferToTexture(source, { .rowPitch = 256 }, set.texture, TextureRegion{ .mip = set.shape.mipTailFirst, .extent = { 1, 1, 1 } });
        submitTo(kit.device, graphics, QueueType::Graphics);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sparse-0014 a shader read of a texture with a newly mapped tile reaches it", "[contract][death]")
{
    // A declared shader access names no texel box, so it reaches every mark of the subresources it names.
    ROJORHI_EXPECT_DEATH("VU-rojo-sparse-0014", [] {
        PassKit kit(QueueType::Graphics, true);
        const SparseSet set = sparse(kit);
        test::transition(kit.list, set.texture, Layout::Undefined, Layout::General);
        submitTo(kit.device, kit.list, QueueType::Graphics);
        kit.device.queue(QueueType::Graphics).mapTiles(std::array{ textureTile(set) });
        CommandList graphics = kit.device.acquireCommandList(QueueType::Graphics, "test.graphics");
        const std::array<ResourceAccess, 1> reads{ ResourceAccess{
            .texture = set.texture, .range = { .mipCount = 1, .layerCount = 1 }, .stage = Stage::ComputeShader, .access = Access::ShaderRead } };
        graphics.setPipeline(kit.compute());
        graphics.declareAccess(reads);
        graphics.dispatch(1, 1, 1);
        submitTo(kit.device, graphics, QueueType::Graphics);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0006 a newly mapped Sparse texture is first used on a Copy list", "[contract][death]")
{
    // A Copy list finds only a texture that is neither placed nor Sparse in General, so the sparse texture is still in
    // Undefined there; the rule reports before the tile's missing discard (VU-rojo-sparse-0014), which it also breaks.
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0006", [] {
        PassKit kit(QueueType::Copy, true);
        const SparseSet set = sparse(kit);
        const Buffer source = kit.buffer(BufferUsage::None, 65536);
        kit.device.queue(QueueType::Copy).mapTiles(std::array{ textureTile(set) });
        kit.list.copyBufferToTexture(source, { .rowPitch = set.shape.width * 4 }, set.texture, firstTile(set));
        submitTo(kit.device, kit.list, QueueType::Copy);
    });
}
