//----------------------------------------------------------------------------------------------------------------------
/// @file AliasingDeathTests.cpp
/// @brief Death tests for the first use of placed memory another resource used earlier on the queue.
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
    /// The bytes of every placed buffer.
    constexpr uint64_t kBytes = 256;

    //==================================================================================================================
    /// Creates a `Memory::Gpu` heap of one mebibyte.
    Heap makeHeap(PassKit& kit)
    {
        return *kit.device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.heap" });
    }

    //==================================================================================================================
    /// Creates a buffer of `kBytes` bytes that a fill may write, placed at `offset` in `heap`.
    Buffer placedBuffer(PassKit& kit, Heap heap, uint64_t offset)
    {
        const BufferDesc desc{ .size = kBytes, .usage = BufferUsage::ShaderWrite, .label = "test.placed.buffer" };
        return *kit.device.createBuffer(desc, {}, Placement{ .heap = heap, .offset = offset });
    }

    //==================================================================================================================
    /// Creates a `PassKit::kSize` square render target placed at `offset` in `heap`.
    Texture placedTexture(PassKit& kit, Heap heap, uint64_t offset)
    {
        const TextureDesc desc{
            .format = Format::RGBA8Unorm, .extent = { PassKit::kSize, PassKit::kSize, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.placed.texture"
        };
        return *kit.device.createTexture(desc, {}, Placement{ .heap = heap, .offset = offset });
    }

    //==================================================================================================================
    /// Records one global barrier from `srcStage` and `srcAccess` to `dstStage` and `dstAccess`.
    void global(CommandList list, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
    {
        const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{
            .srcStage = srcStage, .srcAccess = srcAccess, .dstStage = dstStage, .dstAccess = dstAccess } };
        list.barrier(globals);
    }

    //==================================================================================================================
    /// Records one barrier of every subresource of `texture` from `oldLayout` at `srcStage` to `newLayout` for a copy's
    /// accesses `dstAccess`; the source names the copy's write unless the barrier discards.
    void textureBarrier(CommandList list, Texture texture, Layout oldLayout, Layout newLayout, Stage srcStage, Access dstAccess)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .srcStage = srcStage,
                                                                      .srcAccess = oldLayout == Layout::Undefined ? Access::None : Access::CopyWrite,
                                                                      .dstStage = Stage::Copy,
                                                                      .dstAccess = dstAccess,
                                                                      .oldLayout = oldLayout,
                                                                      .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`, waiting on `wait` and signalling `signal`.
    void submitWith(Device device, CommandList list, QueueType type, std::span<const FenceValue> wait, std::span<const FenceValue> signal)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, wait, signal);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0009 a placed buffer is used after another buffer used its memory with no barrier", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0009", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Buffer first = placedBuffer(kit, heap, 0);
        const Buffer second = placedBuffer(kit, heap, 0);
        kit.list.fillBuffer(first, 0, kBytes, 0);
        kit.list.fillBuffer(second, 0, kBytes, 1);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0009 a placed buffer follows a global barrier whose source misses the texture's use", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0009", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Texture texture = placedTexture(kit, heap, 0);
        const Buffer buffer = placedBuffer(kit, heap, 0);
        textureBarrier(kit.list, texture, Layout::Undefined, Layout::CopyDst, Stage::None, Access::CopyWrite);
        kit.list.clearTexture(texture, {}, {});
        global(kit.list, Stage::ComputeShader, Access::ShaderWrite, Stage::Copy, Access::CopyWrite);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0009 a placed buffer follows a global barrier whose destination misses its use", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0009", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Texture texture = placedTexture(kit, heap, 0);
        const Buffer buffer = placedBuffer(kit, heap, 0);
        textureBarrier(kit.list, texture, Layout::Undefined, Layout::CopyDst, Stage::None, Access::CopyWrite);
        kit.list.clearTexture(texture, {}, {});
        global(kit.list, Stage::Copy, Access::CopyWrite, Stage::ComputeShader, Access::ShaderWrite);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0017 a placed texture is used first after a buffer used its memory", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0017", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Texture texture = placedTexture(kit, heap, 0);
        const Buffer buffer = placedBuffer(kit, heap, 0);
        // The texture's layout permits the clear, and its own barrier orders it; only the buffer's fill came between. The
        // global barrier orders the fill after the discard, which writes the heap bytes too.
        textureBarrier(kit.list, texture, Layout::Undefined, Layout::CopyDst, Stage::None, Access::CopyWrite);
        global(kit.list, Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyWrite);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        kit.list.clearTexture(texture, {}, {});
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0017 a placed texture's first barrier after a buffer used its memory does not discard", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0017", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Texture texture = placedTexture(kit, heap, 0);
        const Buffer buffer = placedBuffer(kit, heap, 0);
        textureBarrier(kit.list, texture, Layout::Undefined, Layout::CopyDst, Stage::None, Access::CopyWrite);
        kit.list.clearTexture(texture, {}, {});
        global(kit.list, Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyWrite);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        textureBarrier(kit.list, texture, Layout::CopyDst, Layout::CopySrc, Stage::Copy, Access::CopyRead);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0017 a placed texture's discard after a buffer used its memory has a source that misses the use", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0017", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Texture texture = placedTexture(kit, heap, 0);
        const Buffer buffer = placedBuffer(kit, heap, 0);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        textureBarrier(kit.list, texture, Layout::Undefined, Layout::CopyDst, Stage::ComputeShader, Access::CopyWrite);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0017 a texture placed after a submitted fill of its bytes discards with a source that misses the fill", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0017", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Buffer buffer = placedBuffer(kit, heap, 0);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        kit.list.end();
        const std::array<CommandList, 1> lists{ kit.list };
        kit.device.queue(QueueType::Graphics).submit(lists);
        const Texture texture = placedTexture(kit, heap, 0);
        kit.list = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
        textureBarrier(kit.list, texture, Layout::Undefined, Layout::CopyDst, Stage::ComputeShader, Access::CopyWrite);
        kit.finish();
    });
}

namespace {
    //==================================================================================================================
    /// Creates a square render target of `size` texels placed at `offset` in `heap`.
    Texture placedTarget(PassKit& kit, Heap heap, uint64_t offset, uint32_t size)
    {
        const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { size, size, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.placed.target" };
        return *kit.device.createTexture(desc, {}, Placement{ .heap = heap, .offset = offset });
    }

    //==================================================================================================================
    /// Records the discard of `texture` to `ColorAttachment` waiting for `srcStage`, then a pass that clears its `size` square.
    void discardAndClear(CommandList list, Texture texture, Stage srcStage, uint32_t size)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .srcStage = srcStage,
                                                                      .dstStage = Stage::ColorAttachment,
                                                                      .dstAccess = Access::ColorAttachmentWrite,
                                                                      .oldLayout = Layout::Undefined,
                                                                      .newLayout = Layout::ColorAttachment } };
        list.barrier({}, {}, barriers);
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = texture, .load = LoadOp::Clear } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, size, size }, .label = "test.pass" });
        list.endRenderPass();
    }

    /// The edge of a small render target, placed after the first buffer's bytes in the heap.
    constexpr uint32_t kSmall = 64;
    /// The edge of a large render target that covers the bytes of both.
    constexpr uint32_t kLarge = 256;

    //==================================================================================================================
    /// Returns the heap offset of the small render target: the first offset past the first buffer's bytes that the device
    /// places a texture at.
    uint64_t smallOffset(PassKit& kit)
    {
        const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { kSmall, kSmall, 1 }, .usage = TextureUsage::RenderTarget };
        const uint64_t alignment = kit.device.placementRequirements(desc).alignment;
        return (kBytes + alignment - 1) / alignment * alignment;
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0017 a discard waits for the newest use of some bytes but not an older use of other bytes", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0017", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Buffer buffer = placedBuffer(kit, heap, 0);
        const Texture small = placedTarget(kit, heap, smallOffset(kit), kSmall);
        const Texture large = placedTarget(kit, heap, 0, kLarge);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        discardAndClear(kit.list, small, Stage::None, kSmall);
        // The large target's bytes hold the buffer's Copy write too, which a ColorAttachment source does not wait for.
        discardAndClear(kit.list, large, Stage::ColorAttachment, kLarge);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0017 a texture placed after uses of disjoint bytes discards waiting for only the newest", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0017", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Buffer buffer = placedBuffer(kit, heap, 0);
        const Texture small = placedTarget(kit, heap, smallOffset(kit), kSmall);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        discardAndClear(kit.list, small, Stage::None, kSmall);
        kit.list.end();
        const std::array<CommandList, 1> lists{ kit.list };
        kit.device.queue(QueueType::Graphics).submit(lists);
        const Texture large = placedTarget(kit, heap, 0, kLarge);
        kit.list = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
        discardAndClear(kit.list, large, Stage::ColorAttachment, kLarge);
        kit.finish();
    });
}

namespace {
    //==================================================================================================================
    /// Records a pass over mip `mip` of `texture`, a `PassKit::kSize` square at mip 0, that loads or clears it as `load` says.
    void renderMip(CommandList list, Texture texture, uint32_t mip, LoadOp load)
    {
        const uint32_t size = PassKit::kSize >> mip;
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = texture, .mip = mip, .load = load } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, size, size }, .label = "test.pass" });
        list.endRenderPass();
    }

    //==================================================================================================================
    /// Records the discard of the mips `range` of `texture` to `ColorAttachment`, waiting for `srcStage`.
    void discardMips(CommandList list, Texture texture, const SubresourceRange& range, Stage srcStage)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .range = range,
                                                                      .srcStage = srcStage,
                                                                      .dstStage = Stage::ColorAttachment,
                                                                      .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                                                                      .oldLayout = Layout::Undefined,
                                                                      .newLayout = Layout::ColorAttachment } };
        list.barrier({}, {}, barriers);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0017 a placed texture's use of one mip does not exempt another mip from discarding", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0017", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const TextureDesc desc{ .format = Format::RGBA8Unorm,
                                .extent = { PassKit::kSize, PassKit::kSize, 1 },
                                .mips = 2,
                                .usage = TextureUsage::RenderTarget,
                                .label = "test.placed.mips" };
        const Texture texture = *kit.device.createTexture(desc, {}, Placement{ .heap = heap, .offset = 0 });
        const Buffer buffer = placedBuffer(kit, heap, 0);
        discardMips(kit.list, texture, {}, Stage::None);
        renderMip(kit.list, texture, 0, LoadOp::Clear);
        renderMip(kit.list, texture, 1, LoadOp::Clear);
        global(kit.list, Stage::ColorAttachment, Access::ColorAttachmentWrite, Stage::Copy, Access::CopyWrite);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        // Mip 0 discards after the fill; mip 1 does not, though the fill overwrote its bytes too.
        discardMips(kit.list, texture, SubresourceRange{ .mipCount = 1 }, Stage::Copy);
        renderMip(kit.list, texture, 0, LoadOp::Clear);
        global(
            kit.list, Stage::ColorAttachment, Access::ColorAttachmentWrite, Stage::ColorAttachment, Access::ColorAttachmentRead | Access::ColorAttachmentWrite
        );
        renderMip(kit.list, texture, 1, LoadOp::Load);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0072 a placed texture is cleared on Graphics, with no discard, after a buffer used its memory on Compute", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0072", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Buffer earlier = placedBuffer(kit, heap, 0);
        const Buffer third = placedBuffer(kit, heap, 0);
        // A Compute list clears only a texture with no attachment usage.
        const TextureDesc desc{
            .format = Format::RGBA8Unorm, .extent = { PassKit::kSize, PassKit::kSize, 1 }, .usage = TextureUsage::Storage, .label = "test.placed.texture"
        };
        const Texture texture = *kit.device.createTexture(desc, {}, Placement{ .heap = heap });
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> first{ FenceValue{ fence, 1 } };
        const std::array<FenceValue, 1> second{ FenceValue{ fence, 2 } };
        kit.list.fillBuffer(earlier, 0, kBytes, 0);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        textureBarrier(compute, texture, Layout::Undefined, Layout::CopyDst, Stage::Copy, Access::CopyWrite);
        compute.clearTexture(texture, {}, {});
        global(compute, Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyWrite);
        compute.fillBuffer(third, 0, kBytes, 2);
        // The fill leaves the texture Undefined on every queue a fence orders after it, so the clear needs a discard first.
        CommandList later = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
        later.clearTexture(texture, {}, {});
        submitWith(kit.device, kit.list, QueueType::Graphics, {}, first);
        submitWith(kit.device, compute, QueueType::Compute, first, second);
        submitWith(kit.device, later, QueueType::Graphics, second, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0007 a placed render target is drawn to on Graphics, with no discard, after a buffer used its memory on Compute", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0007", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Texture texture = placedTexture(kit, heap, 0);
        const Buffer buffer = placedBuffer(kit, heap, 0);
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> rendered{ FenceValue{ fence, 1 } };
        const std::array<FenceValue, 1> filled{ FenceValue{ fence, 2 } };
        test::transition(kit.list, texture, Layout::Undefined, Layout::ColorAttachment);
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = texture } };
        kit.list.beginRenderPass(test::fullPass(colors));
        kit.list.endRenderPass();
        submitWith(kit.device, kit.list, QueueType::Graphics, {}, rendered);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        global(compute, Stage::All, Access::None, Stage::All, Access::CopyWrite);
        compute.fillBuffer(buffer, 0, kBytes, 7);
        submitWith(kit.device, compute, QueueType::Compute, rendered, filled);
        // The fill leaves the target Undefined for Graphics, which waits on it, so the pass needs a discard first.
        CommandList again = kit.device.acquireCommandList(QueueType::Graphics, "test.again");
        again.beginRenderPass(test::fullPass(colors));
        again.endRenderPass();
        submitWith(kit.device, again, QueueType::Graphics, filled, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0009 a placed buffer is filled right after a placed texture over its bytes is discarded", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0009", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Texture texture = placedTexture(kit, heap, 0);
        const Buffer buffer = placedBuffer(kit, heap, 0);
        textureBarrier(kit.list, texture, Layout::Undefined, Layout::CopyDst, Stage::Copy, Access::CopyWrite);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        kit.finish();
    });
}
