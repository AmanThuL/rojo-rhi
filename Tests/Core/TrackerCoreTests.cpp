//----------------------------------------------------------------------------------------------------------------------
/// @file TrackerCoreTests.cpp
/// @brief Tests the events a list logs for the validation tracker and the layout state the tracker keeps per texture.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Tracker/Events.h"
#include "Core/Tracker/Tracker.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <variant>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// A stand-in for a native surface: the Null backend never dereferences it.
    [[maybe_unused]] int g_surface = 0;

    //==================================================================================================================
    /// Records the barrier that moves every subresource of `texture` from `oldLayout` to `newLayout`.
    void move(CommandList list, Texture texture, Layout oldLayout, Layout newLayout)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{
            .texture = texture, .srcStage = Stage::All, .dstStage = Stage::All, .oldLayout = oldLayout, .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Returns the use event at `index` of `list`'s log; the test fails when it is another kind of event.
    [[maybe_unused]] core::UseEvent useAt(CommandList list, size_t index)
    {
        const core::TrackerEvent& event = core::coreOf(list).trackerLog.events.at(index);
        REQUIRE(std::holds_alternative<core::UseEvent>(event));
        return std::get<core::UseEvent>(event);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a device without validation logs no tracker event", "[core]")
{
    PassKit kit;
    const Texture texture = kit.texture();
    const Texture target = kit.target();
    const Buffer source = kit.buffer(BufferUsage::None);
    const Buffer destination = kit.buffer(BufferUsage::None);
    move(kit.list, texture, Layout::Undefined, Layout::CopySrc);
    kit.list.copyBuffer(source, 0, destination, 0, 256);
    kit.list.copyTexture(texture, TextureRegion{ .extent = { 4, 4, 1 } }, target, TextureRegion{ .extent = { 4, 4, 1 } });
    kit.beginSimplePass();
    kit.list.endRenderPass();
    CHECK(core::coreOf(kit.list).trackerLog.events.empty());
    CHECK(core::coreOf(kit.device).tracker().layoutOf(texture, 0, 0) == std::nullopt);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a copy on a validating device logs each endpoint with its stage, access, layouts and rule", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Texture source = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 1, 2);
    const Texture destination = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 1, 2);
    kit.list.copyTexture(source, TextureRegion{ .layer = 1, .extent = { 4, 4, 1 } }, destination, TextureRegion{ .extent = { 4, 4, 1 } });
    REQUIRE(core::coreOf(kit.list).trackerLog.events.size() == 2);
    const core::UseEvent read = useAt(kit.list, 0);
    CHECK(read.texture == source);
    CHECK(read.range.baseMip == 0);
    CHECK(read.range.mipCount == 1);
    CHECK(read.range.baseLayer == 1);
    CHECK(read.range.layerCount == 1);
    CHECK(read.stage == Stage::Copy);
    CHECK(read.access == Access::CopyRead);
    CHECK(read.layouts == core::layoutSet({ Layout::CopySrc, Layout::General }));
    CHECK(read.rule == core::UseRule::Cmd0054);
    CHECK_FALSE(read.copyQueue);
    const core::UseEvent write = useAt(kit.list, 1);
    CHECK(write.texture == destination);
    CHECK(write.access == Access::CopyWrite);
    CHECK(write.layouts == core::layoutSet({ Layout::CopyDst, Layout::General }));
    // Recording does not change the tracker's state; only submission replays the log.
    CHECK(core::coreOf(kit.device).tracker().layoutOf(source, 0, 1) == Layout::Undefined);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a buffer copy on a Copy list logs byte ranges and the queue type", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Copy, true);
    const Buffer source = kit.buffer(BufferUsage::None);
    const Buffer destination = kit.buffer(BufferUsage::None);
    kit.list.copyBuffer(source, 256, destination, 512, 128);
    REQUIRE(core::coreOf(kit.list).trackerLog.events.size() == 2);
    const core::UseEvent read = useAt(kit.list, 0);
    CHECK(read.buffer == source);
    CHECK(read.offset == 256);
    CHECK(read.size == 128);
    CHECK(read.stage == Stage::Copy);
    CHECK(read.access == Access::CopyRead);
    CHECK(read.layouts == 0);
    CHECK(read.copyQueue);
    const core::UseEvent write = useAt(kit.list, 1);
    CHECK(write.buffer == destination);
    CHECK(write.offset == 512);
    CHECK(write.access == Access::CopyWrite);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a render pass logs one use per attachment, which the draws of the pass may complete", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Texture color = kit.target();
    const Texture depth = kit.depthTarget();
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = color, .load = LoadOp::Clear } };
    RenderPassDesc desc = test::fullPass(colors);
    desc.depth = DepthAttachment{ .texture = depth, .load = LoadOp::Load, .store = StoreOp::Store };
    kit.list.beginRenderPass(desc);
    const core::TrackerLog& log = core::coreOf(kit.list).trackerLog;
    REQUIRE(log.events.size() == 2);
    CHECK(log.passUses.size() == 2);
    const core::UseEvent attachment = useAt(kit.list, 0);
    CHECK(attachment.texture == color);
    CHECK(attachment.stage == Stage::ColorAttachment);
    CHECK(attachment.access == Access::ColorAttachmentWrite);
    CHECK(attachment.layouts == core::layoutSet({ Layout::ColorAttachment }));
    CHECK(attachment.rule == core::UseRule::Cmd0007);
    // D32Float has no stencil, so its DontCare stencil operations are ignored and a loaded, stored depth is only tested.
    const core::UseEvent tested = useAt(kit.list, 1);
    CHECK(tested.texture == depth);
    CHECK(tested.stage == Stage::DepthStencil);
    CHECK(tested.access == Access::DepthStencilRead);
    CHECK(tested.layouts == core::layoutSet({ Layout::DepthStencilWrite }));
    CHECK(tested.rule == core::UseRule::Cmd0010);
    kit.list.endRenderPass();
    CHECK(log.passUses.empty());
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a barrier batch logs each of its records", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture();
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{ .srcStage = Stage::Copy, .dstStage = Stage::ComputeShader } };
    const std::array<BufferBarrier, 1> buffers{ BufferBarrier{ .buffer = buffer, .srcStage = Stage::Copy, .dstStage = Stage::ComputeShader } };
    const std::array<TextureBarrier, 1> textures{ TextureBarrier{
        .texture = texture, .srcStage = Stage::All, .dstStage = Stage::All, .oldLayout = Layout::Undefined, .newLayout = Layout::General } };
    kit.list.barrier(globals, buffers, textures);
    const core::TrackerLog& log = core::coreOf(kit.list).trackerLog;
    REQUIRE(log.events.size() == 3);
    CHECK(std::holds_alternative<GlobalBarrier>(log.events[0]));
    CHECK(std::get<BufferBarrier>(log.events[1]).buffer == buffer);
    CHECK(std::get<TextureBarrier>(log.events[2]).newLayout == Layout::General);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("the tracker follows a texture from creation through submission to destruction", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    core::Tracker& tracker = core::coreOf(kit.device).tracker();
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 2, 1);
    CHECK(tracker.layoutOf(texture, 1, 0) == Layout::Undefined);
    const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                  .range = { .baseMip = 1, .mipCount = 1 },
                                                                  .srcStage = Stage::All,
                                                                  .dstStage = Stage::All,
                                                                  .oldLayout = Layout::Undefined,
                                                                  .newLayout = Layout::CopyDst } };
    kit.list.barrier({}, {}, barriers);
    kit.list.end();
    const std::array<CommandList, 1> lists{ kit.list };
    kit.device.queue(QueueType::Graphics).submit(lists);
    CHECK(tracker.layoutOf(texture, 0, 0) == Layout::Undefined);
    CHECK(tracker.layoutOf(texture, 1, 0) == Layout::CopyDst);
    kit.device.destroy(texture);
    CHECK(tracker.layoutOf(texture, 1, 0) == std::nullopt);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("the tracker follows a swapchain image from acquire to present", "[core]")
{
#ifndef NDEBUG
    Device device = *createDevice(DeviceDesc{ .validation = true });
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surface, .label = "core.swapchain" });
    device.beginFrame();
    const Texture image = swapchain.acquire();
    CHECK(core::coreOf(device).tracker().layoutOf(image, 0, 0) == Layout::Undefined);
    CommandList list = device.acquireCommandList(QueueType::Graphics, "core.list");
    move(list, image, Layout::Undefined, Layout::Present);
    list.end();
    const std::array<CommandList, 1> lists{ list };
    device.queue(QueueType::Graphics).submit(lists);
    CHECK(core::coreOf(device).tracker().layoutOf(image, 0, 0) == Layout::Present);
    device.queue(QueueType::Graphics).present(swapchain);
    CHECK(core::coreOf(device).tracker().layoutOf(image, 0, 0) == std::nullopt);
    device.endFrame();
    device.destroy(swapchain);
    destroyDevice(device);
#endif
}
