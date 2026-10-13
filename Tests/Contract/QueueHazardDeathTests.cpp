//----------------------------------------------------------------------------------------------------------------------
/// @file QueueHazardDeathTests.cpp
/// @brief Death tests for uses of the same memory on different queues that no fence orders.
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
    /// The bytes every test writes and reads.
    constexpr uint64_t kBytes = 256;

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`, waiting on `wait` and signalling `signal`.
    void submitTo(Device device, CommandList list, QueueType type, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {})
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, wait, signal);
    }

    //==================================================================================================================
    /// Creates a buffer of `kBytes` bytes that a fill may write.
    Buffer storage(PassKit& kit)
    {
        return kit.buffer(BufferUsage::ShaderWrite, kBytes);
    }

    //==================================================================================================================
    /// Creates a `PassKit::kSize` square texture with `usage`, placed at the start of `heap` when it is not null.
    Texture texture(PassKit& kit, TextureUsage usage, Heap heap = {})
    {
        const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { PassKit::kSize, PassKit::kSize, 1 }, .usage = usage, .label = "test.texture" };
        return *kit.device.createTexture(desc, {}, Placement{ .heap = heap });
    }

    //==================================================================================================================
    /// Records the barrier that moves every subresource of `texture` from `oldLayout` to `newLayout`, ordering every stage on
    /// both sides and making `access` visible.
    void move(CommandList list, Texture texture, Layout oldLayout, Layout newLayout, Access access = Access::CopyRead | Access::CopyWrite)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{
            .texture = texture, .srcStage = Stage::All, .dstStage = Stage::All, .dstAccess = access, .oldLayout = oldLayout, .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Creates a buffer of `kBytes` bytes that a fill may write, placed at the start of `heap`.
    Buffer placedBuffer(PassKit& kit, Heap heap)
    {
        const BufferDesc desc{ .size = kBytes, .usage = BufferUsage::ShaderWrite, .label = "test.placed.buffer" };
        return *kit.device.createBuffer(desc, {}, Placement{ .heap = heap });
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0005 a buffer written on Graphics is read on Compute with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = storage(kit);
        const Buffer other = storage(kit);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.copyBuffer(buffer, 0, other, 0, kBytes);
        submitTo(kit.device, kit.list, QueueType::Graphics);
        submitTo(kit.device, compute, QueueType::Compute);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0005 a buffer read on Compute is written on Graphics with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = storage(kit);
        const Buffer other = storage(kit);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.copyBuffer(buffer, 0, other, 0, kBytes);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        submitTo(kit.device, compute, QueueType::Compute);
        submitTo(kit.device, kit.list, QueueType::Graphics);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0005 a wait on a fence's initial value orders nothing", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = storage(kit);
        const Buffer other = storage(kit);
        const Fence fence = *kit.device.createFence(1, "test.fence");
        const std::array<FenceValue, 1> initial{ FenceValue{ fence, 1 } };
        const std::array<FenceValue, 1> written{ FenceValue{ fence, 2 } };
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.copyBuffer(buffer, 0, other, 0, kBytes);
        submitTo(kit.device, kit.list, QueueType::Graphics, {}, written);
        submitTo(kit.device, compute, QueueType::Compute, initial);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0005 placed buffers over the same bytes are used on two queues with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = *kit.device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.heap" });
        const Buffer first = placedBuffer(kit, heap);
        const Buffer second = placedBuffer(kit, heap);
        kit.list.fillBuffer(first, 0, kBytes, 0);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.fillBuffer(second, 0, kBytes, 1);
        submitTo(kit.device, kit.list, QueueType::Graphics);
        submitTo(kit.device, compute, QueueType::Compute);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0005 a buffer is written on Graphics and on Compute with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = storage(kit);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.fillBuffer(buffer, 0, kBytes, 1);
        submitTo(kit.device, kit.list, QueueType::Graphics);
        submitTo(kit.device, compute, QueueType::Compute);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0005 a texture written on Copy is read on Graphics with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture uploaded = texture(kit, TextureUsage::Sampled);
        const Buffer staging = storage(kit);
        const Buffer readback = kit.buffer(BufferUsage::ShaderWrite, 64 * 1024);
        CommandList copy = kit.device.acquireCommandList(QueueType::Copy, "test.copy");
        move(copy, uploaded, Layout::Undefined, Layout::General);
        copy.copyBufferToTexture(staging, BufferTextureLayout{ .rowPitch = 256 }, uploaded, TextureRegion{ .extent = { PassKit::kSize, 1, 1 } });
        kit.list.copyTextureToBuffer(uploaded, TextureRegion{ .extent = { PassKit::kSize, 1, 1 } }, readback, BufferTextureLayout{ .rowPitch = 256 });
        submitTo(kit.device, copy, QueueType::Copy);
        submitTo(kit.device, kit.list, QueueType::Graphics);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0005 a texture is moved on Compute while Graphics reads it with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture shared = texture(kit, TextureUsage::Sampled);
        const Buffer graphicsCopy = kit.buffer(BufferUsage::ShaderWrite, 64 * 1024);
        const Buffer computeCopy = kit.buffer(BufferUsage::ShaderWrite, 64 * 1024);
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
        const TextureRegion region{ .extent = { PassKit::kSize, 1, 1 } };
        move(kit.list, shared, Layout::Undefined, Layout::General);
        submitTo(kit.device, kit.list, QueueType::Graphics, {}, moved);
        // Graphics reads after the value Compute waits on, so nothing orders the read before the Compute transition.
        CommandList graphics = kit.device.acquireCommandList(QueueType::Graphics, "test.graphics");
        graphics.copyTextureToBuffer(shared, region, graphicsCopy, BufferTextureLayout{ .rowPitch = 256 });
        submitTo(kit.device, graphics, QueueType::Graphics);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        move(compute, shared, Layout::General, Layout::CopySrc, Access::CopyRead);
        compute.copyTextureToBuffer(shared, region, computeCopy, BufferTextureLayout{ .rowPitch = 256 });
        submitTo(kit.device, compute, QueueType::Compute, moved);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0005 a placed texture is discarded on Compute over bytes a buffer wrote on Graphics with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = *kit.device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.heap" });
        const Buffer buffer = placedBuffer(kit, heap);
        const Texture placed = texture(kit, TextureUsage::Storage, heap);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        move(compute, placed, Layout::Undefined, Layout::General);
        submitTo(kit.device, kit.list, QueueType::Graphics);
        submitTo(kit.device, compute, QueueType::Compute);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0005 a placed texture is used on Compute over bytes a buffer wrote on Graphics with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = *kit.device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.heap" });
        const Buffer buffer = placedBuffer(kit, heap);
        const Texture placed = texture(kit, TextureUsage::Storage, heap);
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> cleared{ FenceValue{ fence, 1 } };
        // Compute starts the texture, and Graphics fills the buffer over it only after that, as aliasing needs.
        CommandList first = kit.device.acquireCommandList(QueueType::Compute, "test.compute.first");
        move(first, placed, Layout::Undefined, Layout::General);
        first.clearTexture(placed, {}, {});
        submitTo(kit.device, first, QueueType::Compute, {}, cleared);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        submitTo(kit.device, kit.list, QueueType::Graphics, cleared);
        // Compute uses the texture again, after its own barrier but with no fence after the fill.
        CommandList second = kit.device.acquireCommandList(QueueType::Compute, "test.compute.second");
        const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite } };
        second.barrier(globals);
        second.clearTexture(placed, {}, {});
        submitTo(kit.device, second, QueueType::Compute);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0005 a buffer is filled on Compute over a placed texture Graphics moved, with no fence", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = *kit.device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.heap" });
        const Texture placed = texture(kit, TextureUsage::Storage, heap);
        const Buffer buffer = placedBuffer(kit, heap);
        // The transition writes the texture's heap bytes on Graphics, so the fill on Compute races it.
        move(kit.list, placed, Layout::Undefined, Layout::General);
        submitTo(kit.device, kit.list, QueueType::Graphics);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.fillBuffer(buffer, 0, kBytes, 1);
        submitTo(kit.device, compute, QueueType::Compute);
    });
}
