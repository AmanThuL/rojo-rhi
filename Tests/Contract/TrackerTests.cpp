//----------------------------------------------------------------------------------------------------------------------
/// @file TrackerTests.cpp
/// @brief Tests that the validation tracker accepts correct texture barriers, in queue submission order, on every queue.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Backend.h"
#include "Support/PassKit.h"
#include "Support/UpscaleKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <thread>
#include <vector>

using namespace rojoRHI;
using test::PassKit;
using test::UpscaleKit;

namespace {
    //==================================================================================================================
    /// Records the barrier that moves every subresource of `texture` from `oldLayout` to `newLayout`, with no access on either
    /// side: it orders the transitions of a chain, and no use.
    [[maybe_unused]] void move(CommandList list, Texture texture, Layout oldLayout, Layout newLayout)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{
            .texture = texture, .srcStage = Stage::All, .dstStage = Stage::All, .oldLayout = oldLayout, .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Records the global barrier that orders every copy before it before every copy after it.
    [[maybe_unused]] void copiesDone(CommandList list)
    {
        constexpr Access kCopies = Access::CopyRead | Access::CopyWrite;
        const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = kCopies, .dstStage = Stage::Copy, .dstAccess = kCopies } };
        list.barrier(globals);
    }

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type`.
    [[maybe_unused]] void submit(Device device, CommandList list, QueueType type = QueueType::Graphics)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a chain of texture barriers that each start from the current layout or Undefined is accepted", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage, 2, 2);
    move(kit.list, texture, Layout::Undefined, Layout::CopyDst);
    move(kit.list, texture, Layout::CopyDst, Layout::ShaderRead);
    // One mip moves on its own; the next barrier names each part by the layout it is in.
    const std::array<TextureBarrier, 2> split{
        TextureBarrier{ .texture = texture,
                        .range = { .baseMip = 0, .mipCount = 1 },
                        .srcStage = Stage::All,
                        .dstStage = Stage::All,
                        .oldLayout = Layout::ShaderRead,
                        .newLayout = Layout::ShaderWrite },
        TextureBarrier{ .texture = texture,
                        .range = { .baseMip = 1, .mipCount = 1 },
                        .srcStage = Stage::All,
                        .dstStage = Stage::All,
                        .oldLayout = Layout::ShaderRead,
                        .newLayout = Layout::General },
    };
    kit.list.barrier({}, {}, split);
    // A discard from Undefined is accepted whatever layout the texture is in.
    move(kit.list, texture, Layout::Undefined, Layout::CopySrc);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("the tracker carries layouts from one submission to the next", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    Device device = *createDevice(DeviceDesc{ .validation = true });
    const Texture texture =
        *device.createTexture(TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" });
    // A fence orders the Compute transition after the Graphics one (VU-rojo-queue-0005).
    const Fence fence = *device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
    device.beginFrame();
    CommandList first = device.acquireCommandList(QueueType::Graphics, "test.first");
    move(first, texture, Layout::Undefined, Layout::CopyDst);
    first.end();
    const std::array<CommandList, 1> firstLists{ first };
    device.queue(QueueType::Graphics).submit(firstLists, {}, moved);
    device.endFrame();
    device.beginFrame();
    CommandList second = device.acquireCommandList(QueueType::Compute, "test.second");
    move(second, texture, Layout::CopyDst, Layout::ShaderRead);
    second.end();
    const std::array<CommandList, 1> secondLists{ second };
    const std::array<FenceValue, 1> read{ FenceValue{ fence, 2 } };
    device.queue(QueueType::Compute).submit(secondLists, moved, read);
    // The frame's graphics work joins the compute work before it ends.
    device.queue(QueueType::Graphics).submit({}, read);
    device.endFrame();
    destroyDevice(device);
#endif
}

//======================================================================================================================
TEST_CASE("lists are judged in queue submission order, not in the order they were recorded", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    Device device = *createDevice(DeviceDesc{ .validation = true });
    const Texture texture =
        *device.createTexture(TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" });
    device.beginFrame();
    // The later list is acquired and recorded first; its barrier starts from the layout the earlier list leaves.
    CommandList later = device.acquireCommandList(QueueType::Graphics, "test.later");
    move(later, texture, Layout::CopyDst, Layout::ShaderRead);
    later.end();
    CommandList earlier = device.acquireCommandList(QueueType::Graphics, "test.earlier");
    move(earlier, texture, Layout::Undefined, Layout::CopyDst);
    earlier.end();
    const std::array<CommandList, 2> lists{ earlier, later };
    device.queue(QueueType::Graphics).submit(lists);
    device.endFrame();
    destroyDevice(device);
#endif
}

//======================================================================================================================
TEST_CASE("a texture created with initial data starts in ShaderRead", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const std::vector<std::byte> texels(4 * 4 * 4);
    const std::array<TextureData, 1> initial{ TextureData{ .bytes = texels, .bytesPerRow = 16 } };
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" }, initial
    );
    move(kit.list, texture, Layout::ShaderRead, Layout::CopySrc);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("each acquire of a swapchain image starts a chain from Undefined", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    Device device = *createDevice(DeviceDesc{ .validation = true });
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(), .imageCount = 2, .label = "test.swapchain" });
    Queue queue = device.queue(QueueType::Graphics);
    // Three frames reuse an image of two: its second acquire starts from Undefined again, though it was left in Present.
    for (int frame = 0; frame < 3; ++frame)
    {
        device.beginFrame();
        const Texture image = swapchain.acquire();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        test::transition(list, image, Layout::Undefined, Layout::ColorAttachment);
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = image } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 1, 1 } });
        list.endRenderPass();
        test::transition(list, image, Layout::ColorAttachment, Layout::Present);
        submit(device, list);
        queue.present(swapchain);
        device.endFrame();
    }
    device.destroy(swapchain);
    destroyDevice(device);
#endif
}

//======================================================================================================================
TEST_CASE("two queues submitting from two threads share the tracker without a race", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    Device device = *createDevice(DeviceDesc{ .validation = true });
    device.beginFrame();
    // Each thread creates, transitions, submits and destroys its own textures, so every layout chain is correct whatever
    // order the two queues' submissions interleave in.
    const auto work = [device](QueueType type) mutable {
        for (int round = 0; round < 64; ++round)
        {
            const Texture texture = *device.createTexture(
                TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" }
            );
            CommandList list = device.acquireCommandList(type, "test.list");
            move(list, texture, Layout::Undefined, Layout::CopyDst);
            move(list, texture, Layout::CopyDst, Layout::ShaderRead);
            submit(device, list, type);
            device.destroy(texture);
        }
    };
    std::thread graphics(work, QueueType::Graphics);
    std::thread compute(work, QueueType::Compute);
    graphics.join();
    compute.join();
    // The frame's graphics work joins the compute work before it ends.
    const Fence fence = *device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> joined{ FenceValue{ fence, 1 } };
    device.queue(QueueType::Compute).submit({}, {}, joined);
    device.queue(QueueType::Graphics).submit({}, joined);
    device.endFrame();
    destroyDevice(device);
#endif
}

//======================================================================================================================
TEST_CASE("correct layouts pass the checks of every copy, clear and upscale entry", "[contract]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Texture source = kit.texture();
    const Texture destination = kit.texture();
    const Buffer buffer = kit.buffer(BufferUsage::None, 4096);
    const TextureRegion region{ .extent = { 4, 4, 1 } };
    const BufferTextureLayout layout{ .rowPitch = 256 };
    test::transition(kit.list, source, Layout::Undefined, Layout::CopySrc);
    test::transition(kit.list, destination, Layout::Undefined, Layout::CopyDst);
    kit.list.copyTexture(source, region, destination, region);
    copiesDone(kit.list);
    kit.list.copyBufferToTexture(buffer, layout, destination, region);
    copiesDone(kit.list);
    kit.list.copyTextureToBuffer(source, region, buffer, layout);
    const Texture target = kit.target();
    test::transition(kit.list, target, Layout::Undefined, Layout::CopyDst);
    kit.list.clearTexture(target, {}, {});
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("General permits a copy to and from a texture on a Copy list", "[contract]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Copy, true);
    const Texture texture = kit.texture();
    const Buffer buffer = kit.buffer(BufferUsage::None, 4096);
    // A Copy list finds a new texture in General, so it copies with no barrier.
    kit.list.copyBufferToTexture(buffer, { .rowPitch = 256 }, texture, TextureRegion{ .extent = { 4, 4, 1 } });
    copiesDone(kit.list);
    kit.list.copyTextureToBuffer(texture, TextureRegion{ .extent = { 4, 4, 1 } }, buffer, { .rowPitch = 256 });
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a Copy list's first barrier on a new texture starts from General", "[contract]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Copy, true);
    const Texture texture = kit.texture();
    const Buffer buffer = kit.buffer(BufferUsage::None, 4096);
    // The only texture barrier a Copy list records keeps General, the layout it finds a new texture in.
    const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                  .srcStage = Stage::All,
                                                                  .dstStage = Stage::All,
                                                                  .dstAccess = Access::CopyRead | Access::CopyWrite,
                                                                  .oldLayout = Layout::General,
                                                                  .newLayout = Layout::General } };
    kit.list.barrier({}, {}, barriers);
    kit.list.copyBufferToTexture(buffer, { .rowPitch = 256 }, texture, TextureRegion{ .extent = { 4, 4, 1 } });
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a Copy list uses a mip of a new texture that Graphics never moved", "[contract]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Copy, true);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 2);
    const Buffer buffer = kit.buffer(BufferUsage::None, 4096);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
    CommandList graphics = kit.device.acquireCommandList(QueueType::Graphics, "test.graphics");
    test::transition(graphics, texture, Layout::Undefined, Layout::ShaderRead, SubresourceRange{ .baseMip = 0, .mipCount = 1 });
    graphics.end();
    kit.device.queue(QueueType::Graphics).submit(std::array{ graphics }, {}, moved);
    kit.device.queue(QueueType::Copy).submit({}, moved);
    // Layouts are per subresource: no use or barrier reached mip 1, so the Copy list finds it in General.
    kit.list.copyBufferToTexture(buffer, { .rowPitch = 256 }, texture, TextureRegion{ .mip = 1, .extent = { 4, 4, 1 } });
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a placed texture is discarded on Graphics before its first use on a Copy list", "[contract]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Copy, true);
    const Heap heap = *kit.device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.heap" });
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { PassKit::kSize, PassKit::kSize, 1 }, .usage = TextureUsage::Sampled, .label = "test.placed" },
        {},
        Placement{ .heap = heap, .offset = 0 }
    );
    const Buffer buffer = kit.buffer(BufferUsage::None, 4096);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> discarded{ FenceValue{ fence, 1 } };
    // A Copy list cannot discard and does not find a placed texture in General, so Graphics discards it first.
    CommandList graphics = kit.device.acquireCommandList(QueueType::Graphics, "test.graphics");
    test::transition(graphics, texture, Layout::Undefined, Layout::General);
    graphics.end();
    kit.device.queue(QueueType::Graphics).submit(std::array{ graphics }, {}, discarded);
    kit.device.queue(QueueType::Copy).submit({}, discarded);
    kit.list.copyBufferToTexture(buffer, { .rowPitch = 256 }, texture, TextureRegion{ .extent = { 4, 4, 1 } });
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("an upscale of inputs in ShaderRead and an output in ShaderWrite passes", "[contract]")
{
#ifndef NDEBUG
    UpscaleKit kit(QueueType::Graphics, UpscaleKit::defaultDesc(), true);
    test::transition(kit.kit.list, kit.params.color, Layout::Undefined, Layout::ShaderRead);
    test::transition(kit.kit.list, kit.params.depth, Layout::Undefined, Layout::ShaderRead);
    test::transition(kit.kit.list, kit.params.motion, Layout::Undefined, Layout::ShaderRead);
    test::transition(kit.kit.list, kit.params.output, Layout::Undefined, Layout::ShaderWrite);
    kit.upscale();
    kit.kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a declared read passes in each layout a read through any view permits", "[contract]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage);
    const std::array<ResourceAccess, 1> reads{ ResourceAccess{ .texture = texture, .stage = Stage::ComputeShader, .access = Access::ShaderRead } };
    kit.list.setPipeline(kit.compute());
    for (const Layout layout : { Layout::ShaderRead, Layout::General, Layout::ShaderWrite })
    {
        test::transition(kit.list, texture, Layout::Undefined, layout);
        kit.list.declareAccess(reads);
        kit.list.dispatch(1, 1, 1);
    }
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("an image acquired, drawn to in ColorAttachment and moved to Present is presented", "[contract]")
{
#ifndef NDEBUG
    Device device = *createDevice(DeviceDesc{ .validation = true });
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(), .label = "test.swapchain" });
    device.beginFrame();
    const Texture image = swapchain.acquire();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    move(list, image, Layout::Undefined, Layout::ColorAttachment);
    move(list, image, Layout::ColorAttachment, Layout::Present);
    submit(device, list);
    device.queue(QueueType::Graphics).present(swapchain);
    device.endFrame();
    destroyDevice(device);
#endif
}

//======================================================================================================================
TEST_CASE("copies and clears name subresources by mip and layer, each in its own layout", "[contract]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.target(Format::RGBA8Unorm, 1, 2, 2);
    const Texture other = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 2, 2);
    const auto moveRange = [&](Texture target, uint32_t mip, uint32_t layer, Layout layout) {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = target,
                                                                      .range = { .baseMip = mip, .mipCount = 1, .baseLayer = layer, .layerCount = 1 },
                                                                      .srcStage = Stage::All,
                                                                      .dstStage = Stage::All,
                                                                      .dstAccess = test::accessesOf(layout),
                                                                      .oldLayout = Layout::Undefined,
                                                                      .newLayout = layout } };
        kit.list.barrier({}, {}, barriers);
    };
    // Mip 0 is a source in both layers; mip 1 is a destination in both layers; the other texture is a destination throughout.
    moveRange(texture, 0, 0, Layout::CopySrc);
    moveRange(texture, 0, 1, Layout::CopySrc);
    moveRange(texture, 1, 0, Layout::CopyDst);
    moveRange(texture, 1, 1, Layout::CopyDst);
    moveRange(other, 0, 1, Layout::CopySrc);
    moveRange(other, 1, 0, Layout::CopyDst);
    moveRange(other, 1, 1, Layout::CopyDst);
    moveRange(other, 0, 0, Layout::CopyDst);
    const TextureRegion small{ .extent = { 4, 4, 1 } };
    kit.list.copyTexture(texture, TextureRegion{ .layer = 1, .extent = { 4, 4, 1 } }, texture, TextureRegion{ .mip = 1, .layer = 1, .extent = { 4, 4, 1 } });
    kit.list.copyTexture(texture, small, texture, TextureRegion{ .mip = 1, .extent = { 4, 4, 1 } });
    kit.list.copyTexture(other, TextureRegion{ .layer = 1, .extent = { 4, 4, 1 } }, other, small);
    copiesDone(kit.list);
    kit.list.clearTexture(texture, SubresourceRange{ .baseMip = 1, .mipCount = 1, .baseLayer = 1, .layerCount = 1 }, {});
    kit.list.clearTexture(texture, SubresourceRange{ .baseMip = 1, .mipCount = 1, .baseLayer = 0, .layerCount = 1 }, {});
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a pass with color, depth and resolve attachments in the layouts they need is accepted", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture multisampled = kit.target(Format::RGBA8Unorm, 4);
    const Texture resolve = kit.target();
    const Texture depth = kit.depthTarget(Format::D32Float, 4);
    test::transition(kit.list, multisampled, Layout::Undefined, Layout::ColorAttachment);
    test::transition(kit.list, resolve, Layout::Undefined, Layout::ColorAttachment);
    test::transition(kit.list, depth, Layout::Undefined, Layout::DepthStencilWrite);
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = multisampled, .resolve = resolve } };
    RenderPassDesc desc = test::fullPass(colors);
    desc.depth = DepthAttachment{ .texture = depth };
    kit.list.beginRenderPass(desc);
    kit.list.endRenderPass();
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a pass on mip 1 and layer 1 needs only that subresource in ColorAttachment", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.target(Format::RGBA8Unorm, 1, 2, 2);
    const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                  .range = { .baseMip = 1, .mipCount = 1, .baseLayer = 1, .layerCount = 1 },
                                                                  .srcStage = Stage::All,
                                                                  .dstStage = Stage::All,
                                                                  .dstAccess = test::accessesOf(Layout::ColorAttachment),
                                                                  .oldLayout = Layout::Undefined,
                                                                  .newLayout = Layout::ColorAttachment } };
    kit.list.barrier({}, {}, barriers);
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = texture, .mip = 1, .layer = 1 } };
    kit.list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 32, 32 }, .label = "test.pass" });
    kit.list.endRenderPass();
    kit.finish();
#endif
}
