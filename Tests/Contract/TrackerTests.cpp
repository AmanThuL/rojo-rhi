//----------------------------------------------------------------------------------------------------------------------
/// @file TrackerTests.cpp
/// @brief Tests that the validation tracker accepts correct texture barriers, in queue submission order, on every queue.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <thread>
#include <vector>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// A stand-in for a native surface: the Null backend never dereferences it.
    [[maybe_unused]] int g_surface = 0;

    //==================================================================================================================
    /// Records the barrier that moves every subresource of `texture` from `oldLayout` to `newLayout`, with no access on either
    /// side.
    [[maybe_unused]] void move(CommandList list, Texture texture, Layout oldLayout, Layout newLayout)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{
            .texture = texture, .srcStage = Stage::All, .dstStage = Stage::All, .oldLayout = oldLayout, .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
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
    device.beginFrame();
    CommandList first = device.acquireCommandList(QueueType::Graphics, "test.first");
    move(first, texture, Layout::Undefined, Layout::CopyDst);
    submit(device, first);
    device.endFrame();
    device.beginFrame();
    CommandList second = device.acquireCommandList(QueueType::Compute, "test.second");
    move(second, texture, Layout::CopyDst, Layout::ShaderRead);
    submit(device, second, QueueType::Compute);
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
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surface, .imageCount = 2, .label = "test.swapchain" });
    Queue queue = device.queue(QueueType::Graphics);
    // Three frames reuse an image of two: its second acquire starts from Undefined again, though it was left in Present.
    for (int frame = 0; frame < 3; ++frame)
    {
        device.beginFrame();
        const Texture image = swapchain.acquire();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        move(list, image, Layout::Undefined, Layout::ColorAttachment);
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = image } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 1, 1 } });
        list.endRenderPass();
        move(list, image, Layout::ColorAttachment, Layout::Present);
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
    device.endFrame();
    destroyDevice(device);
#endif
}
