//----------------------------------------------------------------------------------------------------------------------
/// @file AliasingDeathTests.cpp
/// @brief Death tests for the first use of placed memory another resource used earlier on the queue.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

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
        // The texture's layout permits the clear, and its own barrier orders it; only the buffer's fill came between.
        textureBarrier(kit.list, texture, Layout::Undefined, Layout::CopyDst, Stage::None, Access::CopyWrite);
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

    /// The edge of a small render target, whose 16384 bytes follow the first buffer's in the heap.
    constexpr uint32_t kSmall = 64;
    /// The edge of a large render target that covers the bytes of both.
    constexpr uint32_t kLarge = 256;
    /// The heap offset of the small render target.
    constexpr uint64_t kSmallOffset = 16384;
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0017 a discard waits for the newest use of some bytes but not an older use of other bytes", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0017", [] {
        PassKit kit(QueueType::Graphics, true);
        const Heap heap = makeHeap(kit);
        const Buffer buffer = placedBuffer(kit, heap, 0);
        const Texture small = placedTarget(kit, heap, kSmallOffset, kSmall);
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
        const Texture small = placedTarget(kit, heap, kSmallOffset, kSmall);
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
