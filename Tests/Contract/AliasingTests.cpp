//----------------------------------------------------------------------------------------------------------------------
/// @file AliasingTests.cpp
/// @brief Tests that the validation tracker accepts aliased placed resources behind the barriers aliasing needs.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// The bytes of every placed buffer.
    [[maybe_unused]] constexpr uint64_t kBytes = 256;

    //==================================================================================================================
    /// Creates a `Memory::Gpu` heap of one mebibyte.
    [[maybe_unused]] Heap makeHeap(PassKit& kit)
    {
        return *kit.device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.heap" });
    }

    //==================================================================================================================
    /// Creates a buffer of `size` bytes that a fill may write, placed at `offset` in `heap`.
    [[maybe_unused]] Buffer placedBuffer(PassKit& kit, Heap heap, uint64_t offset, uint64_t size = kBytes)
    {
        const BufferDesc desc{ .size = size, .usage = BufferUsage::ShaderWrite, .label = "test.placed.buffer" };
        return *kit.device.createBuffer(desc, {}, Placement{ .heap = heap, .offset = offset });
    }

    //==================================================================================================================
    /// Returns the description of a `PassKit::kSize` square texture with `usage`, a render target by default.
    [[maybe_unused]] TextureDesc targetDesc(TextureUsage usage = TextureUsage::RenderTarget)
    {
        return TextureDesc{ .format = Format::RGBA8Unorm, .extent = { PassKit::kSize, PassKit::kSize, 1 }, .usage = usage, .label = "test.placed.texture" };
    }

    //==================================================================================================================
    /// Creates a `PassKit::kSize` square texture with `usage`, a render target by default, placed at `offset` in `heap`.
    [[maybe_unused]] Texture placedTexture(PassKit& kit, Heap heap, uint64_t offset, TextureUsage usage = TextureUsage::RenderTarget)
    {
        return *kit.device.createTexture(targetDesc(usage), {}, Placement{ .heap = heap, .offset = offset });
    }

    //==================================================================================================================
    /// Records one global barrier from `srcStage` and `srcAccess` to `dstStage` and `dstAccess`.
    [[maybe_unused]] void global(CommandList list, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
    {
        const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{
            .srcStage = srcStage, .srcAccess = srcAccess, .dstStage = dstStage, .dstAccess = dstAccess } };
        list.barrier(globals);
    }

    //==================================================================================================================
    /// Records the discard of every subresource of `texture` to `CopyDst` that waits for the copy stage, as the first use of
    /// memory a copy used earlier needs.
    [[maybe_unused]] void discardAfterCopy(CommandList list, Texture texture)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .srcStage = Stage::Copy,
                                                                      .dstStage = Stage::Copy,
                                                                      .dstAccess = Access::CopyWrite,
                                                                      .oldLayout = Layout::Undefined,
                                                                      .newLayout = Layout::CopyDst } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`.
    [[maybe_unused]] void submit(Device device, CommandList list, QueueType type)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a placed buffer and texture alternate on one heap range behind the barriers aliasing needs", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer buffer = placedBuffer(kit, heap, 0);
    const Texture texture = placedTexture(kit, heap, 0);
    CommandList later = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
    for (CommandList list : { kit.list, later })
    {
        list.fillBuffer(buffer, 0, kBytes, 0);
        // The texture's first use since the fill discards, waiting for the copy stage that filled.
        discardAfterCopy(list, texture);
        list.clearTexture(texture, {}, {});
        // The buffer's first use since the clear follows a global barrier from the clear to the fill.
        global(list, Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyWrite);
    }
    submit(kit.device, kit.list, QueueType::Graphics);
    submit(kit.device, later, QueueType::Graphics);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("placed resources that share no heap bytes need no barrier between them", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const uint64_t textureOffset = kit.device.placementRequirements(targetDesc()).alignment;
    const Buffer first = placedBuffer(kit, heap, 0);
    const Buffer second = placedBuffer(kit, heap, kBytes);
    const Texture texture = placedTexture(kit, heap, textureOffset);
    // A buffer over the bytes of both buffers that touches only the second one's.
    const Buffer wide = placedBuffer(kit, heap, 0, 2 * kBytes);
    kit.list.fillBuffer(first, 0, kBytes, 0);
    kit.list.fillBuffer(second, 0, kBytes, 1);
    discardAfterCopy(kit.list, texture);
    kit.list.clearTexture(texture, {}, {});
    global(kit.list, Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyWrite);
    kit.list.fillBuffer(wide, kBytes, kBytes, 2);
    // The first buffer's bytes were used by no other resource since its fill; its own barrier orders this fill.
    kit.list.fillBuffer(first, 0, kBytes, 3);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("aliased placed resources used on different queues are not compared by the aliasing rules", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer graphicsBuffer = placedBuffer(kit, heap, 0);
    const Buffer computeBuffer = placedBuffer(kit, heap, 0);
    const Texture texture = placedTexture(kit, heap, 0);
    discardAfterCopy(kit.list, texture);
    kit.list.clearTexture(texture, {}, {});
    global(kit.list, Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyWrite);
    kit.list.fillBuffer(graphicsBuffer, 0, kBytes, 0);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.fillBuffer(computeBuffer, 0, kBytes, 1);
    CommandList later = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
    global(later, Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyWrite);
    later.fillBuffer(graphicsBuffer, 0, kBytes, 2);
    submit(kit.device, kit.list, QueueType::Graphics);
    submit(kit.device, compute, QueueType::Compute);
    submit(kit.device, later, QueueType::Graphics);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a texture placed over bytes a buffer used first discards, waiting for that use", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer buffer = placedBuffer(kit, heap, 0);
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    submit(kit.device, kit.list, QueueType::Graphics);
    // Created after the fill was submitted, the texture still follows it on the queue.
    const Texture texture = placedTexture(kit, heap, 0);
    CommandList later = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
    discardAfterCopy(later, texture);
    later.clearTexture(texture, {}, {});
    submit(kit.device, later, QueueType::Graphics);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

namespace {
    //==================================================================================================================
    /// Records a pass that clears and stores the whole of `texture`, already in `ColorAttachment`, with no draw.
    [[maybe_unused]] void clearPass(CommandList list, Texture texture)
    {
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = texture, .load = LoadOp::Clear } };
        list.beginRenderPass(test::fullPass(colors));
        list.endRenderPass();
    }

    //==================================================================================================================
    /// Records the discard of every subresource of `texture` to `newLayout`, waiting for `srcStage` and making it visible to
    /// `dstStage` and `dstAccess`.
    [[maybe_unused]] void discard(CommandList list, Texture texture, Stage srcStage, Layout newLayout, Stage dstStage, Access dstAccess)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{
            .texture = texture, .srcStage = srcStage, .dstStage = dstStage, .dstAccess = dstAccess, .oldLayout = Layout::Undefined, .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`, waiting on `wait` and signalling `signal`.
    [[maybe_unused]] void submitWith(Device device, CommandList list, QueueType type, std::span<const FenceValue> wait, std::span<const FenceValue> signal)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, wait, signal);
    }
} // namespace

//======================================================================================================================
TEST_CASE("two transient render targets ping-pong over one heap range behind discards", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Texture first = placedTexture(kit, heap, 0);
    const Texture second = placedTexture(kit, heap, 0);
    discard(kit.list, first, Stage::None, Layout::ColorAttachment, Stage::ColorAttachment, Access::ColorAttachmentWrite);
    clearPass(kit.list, first);
    discard(kit.list, second, Stage::ColorAttachment, Layout::CopyDst, Stage::Copy, Access::CopyWrite);
    kit.list.clearTexture(second, {}, {});
    // The discard waits for the clear only; the aliasing chain orders the first texture's own pass before it.
    discard(kit.list, first, Stage::Copy, Layout::ColorAttachment, Stage::ColorAttachment, Access::ColorAttachmentWrite);
    clearPass(kit.list, first);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a placed buffer's own earlier fill is ordered by the aliasing chain through a texture", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer buffer = placedBuffer(kit, heap, 0);
    const Texture texture = placedTexture(kit, heap, 0);
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    discard(kit.list, texture, Stage::Copy, Layout::ColorAttachment, Stage::ColorAttachment, Access::ColorAttachmentWrite);
    clearPass(kit.list, texture);
    global(kit.list, Stage::ColorAttachment, Access::ColorAttachmentWrite, Stage::Copy, Access::CopyWrite);
    kit.list.fillBuffer(buffer, 0, kBytes, 1);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a placed buffer used on another queue since an alias's use on this one is not compared with it", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer earlier = placedBuffer(kit, heap, 0);
    const Buffer handed = placedBuffer(kit, heap, 0);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> first{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> second{ FenceValue{ fence, 2 } };
    kit.list.fillBuffer(earlier, 0, kBytes, 0);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.fillBuffer(handed, 0, kBytes, 1);
    CommandList later = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
    later.fillBuffer(handed, 0, kBytes, 2);
    submitWith(kit.device, kit.list, QueueType::Graphics, {}, first);
    submitWith(kit.device, compute, QueueType::Compute, first, second);
    submitWith(kit.device, later, QueueType::Graphics, second, {});
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a placed texture discarded on another queue since an alias's use is not compared with it", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer buffer = placedBuffer(kit, heap, 0);
    // A Compute list clears only a texture with no attachment usage.
    const Texture texture = placedTexture(kit, heap, 0, TextureUsage::Storage);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> first{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> second{ FenceValue{ fence, 2 } };
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    discardAfterCopy(compute, texture);
    compute.clearTexture(texture, {}, {});
    CommandList later = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
    later.clearTexture(texture, {}, {});
    submitWith(kit.device, kit.list, QueueType::Graphics, {}, first);
    submitWith(kit.device, compute, QueueType::Compute, first, second);
    submitWith(kit.device, later, QueueType::Graphics, second, {});
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a placed buffer's own uses are ordered by its own barriers and not by the aliasing rule", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer buffer = placedBuffer(kit, heap, 0);
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    const std::array<BufferBarrier, 1> barriers{ BufferBarrier{
        .buffer = buffer, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite } };
    kit.list.barrier({}, barriers);
    kit.list.fillBuffer(buffer, 0, kBytes, 1);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a placed texture's own uses are ordered by its own barriers and not by the aliasing rule", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Texture texture = placedTexture(kit, heap, 0);
    discardAfterCopy(kit.list, texture);
    kit.list.clearTexture(texture, {}, {});
    const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                  .srcStage = Stage::Copy,
                                                                  .srcAccess = Access::CopyWrite,
                                                                  .dstStage = Stage::Copy,
                                                                  .dstAccess = Access::CopyWrite,
                                                                  .oldLayout = Layout::CopyDst,
                                                                  .newLayout = Layout::CopyDst } };
    kit.list.barrier({}, {}, barriers);
    kit.list.clearTexture(texture, {}, {});
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a placed texture's discard waits only for the latest use another resource made of its memory", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer buffer = placedBuffer(kit, heap, 0);
    const Texture sampled = placedTexture(kit, heap, 0, TextureUsage::Sampled);
    const Texture target = placedTexture(kit, heap, 0);
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    global(kit.list, Stage::Copy, Access::CopyWrite, Stage::FragmentShader, Access::ShaderRead);
    discard(kit.list, sampled, Stage::Copy, Layout::ShaderRead, Stage::FragmentShader, Access::ShaderRead);
    kit.readyToDraw();
    const std::array<ResourceAccess, 1> reads{ ResourceAccess{ .texture = sampled, .stage = Stage::FragmentShader, .access = Access::ShaderRead } };
    kit.list.declareAccess(reads);
    kit.list.draw(3);
    kit.list.endRenderPass();
    // The fill is ordered before the fragment read by the global barrier, and the read before the discard by its source.
    discard(kit.list, target, Stage::FragmentShader, Layout::CopyDst, Stage::Copy, Access::CopyWrite);
    kit.list.clearTexture(target, {}, {});
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a placed buffer used on another queue since an alias's use stays exempt after a third buffer's later use there", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer earlier = placedBuffer(kit, heap, 0);
    const Buffer handed = placedBuffer(kit, heap, 0);
    const Buffer third = placedBuffer(kit, heap, 0);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> first{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> second{ FenceValue{ fence, 2 } };
    kit.list.fillBuffer(earlier, 0, kBytes, 0);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.fillBuffer(handed, 0, kBytes, 1);
    global(compute, Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyWrite);
    compute.fillBuffer(third, 0, kBytes, 2);
    CommandList later = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
    later.fillBuffer(handed, 0, kBytes, 3);
    submitWith(kit.device, kit.list, QueueType::Graphics, {}, first);
    submitWith(kit.device, compute, QueueType::Compute, first, second);
    submitWith(kit.device, later, QueueType::Graphics, second, {});
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a placed texture used on another queue since an alias's use stays exempt after a buffer's later use there", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer earlier = placedBuffer(kit, heap, 0);
    const Buffer third = placedBuffer(kit, heap, 0);
    const Texture texture = placedTexture(kit, heap, 0, TextureUsage::Storage);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> first{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> second{ FenceValue{ fence, 2 } };
    kit.list.fillBuffer(earlier, 0, kBytes, 0);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    discardAfterCopy(compute, texture);
    compute.clearTexture(texture, {}, {});
    global(compute, Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyWrite);
    compute.fillBuffer(third, 0, kBytes, 2);
    CommandList later = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
    later.clearTexture(texture, {}, {});
    submitWith(kit.device, kit.list, QueueType::Graphics, {}, first);
    submitWith(kit.device, compute, QueueType::Compute, first, second);
    submitWith(kit.device, later, QueueType::Graphics, second, {});
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}
