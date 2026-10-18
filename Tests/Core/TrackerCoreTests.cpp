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
#include "Support/UpscaleKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <variant>

using namespace rojoRHI;
using test::PassKit;
using test::UpscaleKit;

namespace {
    /// A stand-in for a native surface: the Null backend never dereferences it.
    [[maybe_unused]] int g_surface = 0;

    //==================================================================================================================
    /// Records the barrier that moves every subresource of `texture` from `oldLayout` to `newLayout`, ordering the uses after it.
    void move(CommandList list, Texture texture, Layout oldLayout, Layout newLayout)
    {
        test::transition(list, texture, oldLayout, newLayout);
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
    move(kit.list, source, Layout::Undefined, Layout::CopySrc);
    move(kit.list, destination, Layout::Undefined, Layout::CopyDst);
    const size_t before = core::coreOf(kit.list).trackerLog.events.size();
    kit.list.copyTexture(source, TextureRegion{ .layer = 1, .extent = { 4, 4, 1 } }, destination, TextureRegion{ .extent = { 4, 4, 1 } });
    REQUIRE(core::coreOf(kit.list).trackerLog.events.size() == before + 2);
    const core::UseEvent read = useAt(kit.list, before);
    CHECK(read.texture == source);
    CHECK(read.range.baseMip == 0);
    CHECK(read.range.mipCount == 1);
    CHECK(read.range.baseLayer == 1);
    CHECK(read.range.layerCount == 1);
    CHECK(read.stage == Stage::Copy);
    CHECK(read.access == Access::CopyRead);
    CHECK(read.layouts == core::layoutSet({ Layout::CopySrc, Layout::General }));
    CHECK(read.rule == core::UseRule::Cmd0054);
    const core::UseEvent write = useAt(kit.list, before + 1);
    CHECK(write.texture == destination);
    CHECK(write.access == Access::CopyWrite);
    CHECK(write.layouts == core::layoutSet({ Layout::CopyDst, Layout::General }));
    // Recording does not change the tracker's state; only submission replays the log.
    CHECK(core::coreOf(kit.device).tracker().layoutOf(source, 0, 1) == Layout::Undefined);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a buffer copy on a Copy list logs byte ranges", "[core]")
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
    const core::UseEvent write = useAt(kit.list, 1);
    CHECK(write.buffer == destination);
    CHECK(write.offset == 512);
    CHECK(write.access == Access::CopyWrite);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a loaded color attachment reads, and a cleared one does not unless a draw blends", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const std::array<ColorAttachment, 2> colors{ ColorAttachment{ .texture = kit.target(), .load = LoadOp::Load },
                                                 ColorAttachment{ .texture = kit.target(), .load = LoadOp::Clear } };
    kit.list.beginRenderPass(test::fullPass(colors));
    CHECK(useAt(kit.list, 0).access == (Access::ColorAttachmentRead | Access::ColorAttachmentWrite));
    CHECK(useAt(kit.list, 1).access == Access::ColorAttachmentWrite);
    kit.list.endRenderPass();
#endif
}

//======================================================================================================================
TEST_CASE("a draw whose pipeline blends a color target adds the read to that attachment only", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const std::array<ColorAttachment, 2> colors{ ColorAttachment{ .texture = kit.target(), .load = LoadOp::Clear },
                                                 ColorAttachment{ .texture = kit.target(), .load = LoadOp::Clear } };
    kit.list.beginRenderPass(test::fullPass(colors));
    const std::array<ColorTarget, 2> targets{ ColorTarget{ .format = Format::RGBA8Unorm },
                                              ColorTarget{ .format = Format::RGBA8Unorm, .blend = { .enable = true } } };
    kit.list.setPipeline(kit.graphicsWith(targets, Format::Undefined, {}));
    kit.list.setViewport({ .width = static_cast<float>(PassKit::kSize), .height = static_cast<float>(PassKit::kSize) });
    kit.list.setScissor({ 0, 0, PassKit::kSize, PassKit::kSize });
    kit.list.draw(3);
    CHECK(useAt(kit.list, 0).access == Access::ColorAttachmentWrite);
    CHECK(useAt(kit.list, 1).access == (Access::ColorAttachmentRead | Access::ColorAttachmentWrite));
    kit.list.endRenderPass();
#endif
}

//======================================================================================================================
TEST_CASE("a depth test pass writes depth only when a draw's pipeline writes depth or a stencil operation", "[core]")
{
#ifndef NDEBUG
    const auto record = [](const DepthStencilState& state, Format format) {
        PassKit kit(QueueType::Graphics, true);
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target() } };
        RenderPassDesc desc = test::fullPass(colors);
        desc.depth = DepthAttachment{
            .texture = kit.depthTarget(format), .load = LoadOp::Load, .store = StoreOp::Store, .stencilLoad = LoadOp::Load, .stencilStore = StoreOp::Store
        };
        kit.list.beginRenderPass(desc);
        const std::array<ColorTarget, 1> targets{ ColorTarget{ .format = Format::RGBA8Unorm } };
        kit.list.setPipeline(kit.graphicsWith(targets, format, state));
        kit.list.setViewport({ .width = static_cast<float>(PassKit::kSize), .height = static_cast<float>(PassKit::kSize) });
        kit.list.setScissor({ 0, 0, PassKit::kSize, PassKit::kSize });
        kit.list.draw(3);
        const Access access = useAt(kit.list, 1).access;
        kit.list.endRenderPass();
        return access;
    };
    CHECK(record({ .depthTest = true }, Format::D32Float) == Access::DepthStencilRead);
    CHECK(record({ .depthTest = true, .depthWrite = true }, Format::D32Float) == (Access::DepthStencilRead | Access::DepthStencilWrite));
    // A stencil operation other than Keep writes; a stencil test that only keeps does not.
    const StencilFaceState replace{ .pass = StencilOp::Replace };
    CHECK(record({ .stencilTest = true, .front = replace }, Format::D32FloatS8) == (Access::DepthStencilRead | Access::DepthStencilWrite));
    CHECK(record({ .stencilTest = true }, Format::D32FloatS8) == Access::DepthStencilRead);
#endif
}

//======================================================================================================================
TEST_CASE("the load and store operations of a depth attachment decide whether the pass writes it", "[core]")
{
#ifndef NDEBUG
    const auto access = [](const DepthAttachment& depth) {
        PassKit kit(QueueType::Graphics, true);
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target() } };
        RenderPassDesc desc = test::fullPass(colors);
        desc.depth = depth;
        desc.depth.texture =
            kit.depthTarget(depth.stencilLoad == LoadOp::DontCare && depth.stencilStore == StoreOp::DontCare ? Format::D32Float : Format::D32FloatS8);
        kit.list.beginRenderPass(desc);
        const Access logged = useAt(kit.list, 1).access;
        kit.list.endRenderPass();
        return logged;
    };
    constexpr Access kTested = Access::DepthStencilRead;
    constexpr Access kWritten = Access::DepthStencilRead | Access::DepthStencilWrite;
    const DepthAttachment keep{ .load = LoadOp::Load, .store = StoreOp::Store, .stencilLoad = LoadOp::Load, .stencilStore = StoreOp::Store };
    CHECK(access(keep) == kTested);
    DepthAttachment cleared = keep;
    cleared.load = LoadOp::Clear;
    CHECK(access(cleared) == kWritten);
    DepthAttachment discarded = keep;
    discarded.load = LoadOp::DontCare;
    CHECK(access(discarded) == kWritten);
    DepthAttachment dropped = keep;
    dropped.store = StoreOp::DontCare;
    CHECK(access(dropped) == kWritten);
    DepthAttachment stencilCleared = keep;
    stencilCleared.stencilLoad = LoadOp::Clear;
    CHECK(access(stencilCleared) == kWritten);
    DepthAttachment stencilDropped = keep;
    stencilDropped.stencilStore = StoreOp::DontCare;
    CHECK(access(stencilDropped) == kWritten);
    // A depth-only format ignores the stencil fields, which default to DontCare.
    const DepthAttachment depthOnly{ .load = LoadOp::Load, .store = StoreOp::Store };
    CHECK(access(depthOnly) == kTested);
#endif
}

//======================================================================================================================
TEST_CASE("a stencil operation with the stencil test off adds no write", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target() } };
    RenderPassDesc desc = test::fullPass(colors);
    desc.depth = DepthAttachment{ .texture = kit.depthTarget(Format::D32FloatS8),
                                  .load = LoadOp::Load,
                                  .store = StoreOp::Store,
                                  .stencilLoad = LoadOp::Load,
                                  .stencilStore = StoreOp::Store };
    kit.list.beginRenderPass(desc);
    const std::array<ColorTarget, 1> targets{ ColorTarget{ .format = Format::RGBA8Unorm } };
    kit.list.setPipeline(kit.graphicsWith(targets, Format::D32FloatS8, { .front = { .pass = StencilOp::Replace } }));
    kit.list.setViewport({ .width = static_cast<float>(PassKit::kSize), .height = static_cast<float>(PassKit::kSize) });
    kit.list.setScissor({ 0, 0, PassKit::kSize, PassKit::kSize });
    kit.list.draw(3);
    CHECK(useAt(kit.list, 1).access == Access::DepthStencilRead);
    kit.list.endRenderPass();
#endif
}

//======================================================================================================================
TEST_CASE("a resolve target between two blended color attachments does not shift the color index", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const std::array<ColorAttachment, 3> colors{ ColorAttachment{
                                                     .texture = kit.target(Format::RGBA8Unorm, 4), .load = LoadOp::Clear, .resolve = kit.target() },
                                                 ColorAttachment{ .texture = kit.target(Format::RGBA8Unorm, 4), .load = LoadOp::Clear },
                                                 ColorAttachment{ .texture = kit.target(Format::RGBA8Unorm, 4), .load = LoadOp::Clear } };
    RenderPassDesc desc = test::fullPass(colors);
    kit.list.beginRenderPass(desc);
    // The events are color 0, its resolve, color 1 and color 2; only colors 1 and 2 blend.
    const ColorTarget plain{ .format = Format::RGBA8Unorm };
    const ColorTarget blended{ .format = Format::RGBA8Unorm, .blend = { .enable = true } };
    const std::array<ColorTarget, 3> targets{ plain, blended, blended };
    kit.list.setPipeline(kit.graphicsWith(targets, Format::Undefined, {}, 4));
    kit.list.setViewport({ .width = static_cast<float>(PassKit::kSize), .height = static_cast<float>(PassKit::kSize) });
    kit.list.setScissor({ 0, 0, PassKit::kSize, PassKit::kSize });
    kit.list.draw(3);
    CHECK(useAt(kit.list, 0).access == Access::ColorAttachmentWrite);
    CHECK(useAt(kit.list, 1).access == Access::Resolve);
    CHECK(useAt(kit.list, 2).access == (Access::ColorAttachmentRead | Access::ColorAttachmentWrite));
    CHECK(useAt(kit.list, 3).access == (Access::ColorAttachmentRead | Access::ColorAttachmentWrite));
    kit.list.endRenderPass();
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
    // The records of one call belong to one command, so the tracker never chains them with each other.
    REQUIRE(log.commands.size() == 3);
    CHECK(log.commands[0] == log.commands[1]);
    CHECK(log.commands[1] == log.commands[2]);
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

//======================================================================================================================
TEST_CASE("indexed draws log their index reads", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Buffer indices = kit.buffer(BufferUsage::Index, 256);
    kit.readyToDraw();
    const size_t before = core::coreOf(kit.list).trackerLog.events.size();
    kit.list.drawIndexed(indices, IndexType::UInt32, 8, 1, 2);
    kit.list.drawIndexedIndirect(indices, IndexType::UInt16, kit.device.address(kit.buffer(BufferUsage::IndirectArgs, 1024)), 1);
    const core::UseEvent direct = useAt(kit.list, before);
    CHECK(direct.buffer == indices);
    CHECK(direct.offset == 8);
    CHECK(direct.size == 32);
    CHECK(direct.stage == Stage::IndexInput);
    CHECK(direct.access == Access::IndexRead);
    CHECK(direct.layouts == 0);
    const core::UseEvent whole = useAt(kit.list, before + 1);
    CHECK(whole.buffer == indices);
    CHECK(whole.offset == 0);
    CHECK(whole.size == kWholeSize);
    CHECK(whole.access == Access::IndexRead);
    kit.list.endRenderPass();
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("indirect commands log their arguments and count", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Buffer args = kit.buffer(BufferUsage::IndirectArgs, 1024);
    const Addr records = kit.device.address(args, 16);
    const Addr count = kit.device.address(args, 1020);
    kit.readyToDraw();
    const size_t before = core::coreOf(kit.list).trackerLog.events.size();
    kit.list.drawIndirect(records, 4, count);
    REQUIRE(core::coreOf(kit.list).trackerLog.events.size() == before + 2);
    const core::UseEvent arguments = useAt(kit.list, before);
    CHECK(arguments.buffer == args);
    CHECK(arguments.offset == 16);
    CHECK(arguments.size == 4 * sizeof(DrawIndirectArgs));
    CHECK(arguments.stage == Stage::IndirectArgs);
    CHECK(arguments.access == Access::IndirectArgs);
    const core::UseEvent counted = useAt(kit.list, before + 1);
    CHECK(counted.buffer == args);
    CHECK(counted.offset == 1020);
    CHECK(counted.size == sizeof(uint32_t));
    kit.list.endRenderPass();
    kit.list.setPipeline(kit.compute());
    kit.list.dispatchIndirect(records);
    const core::UseEvent group = useAt(kit.list, before + 2);
    CHECK(group.offset == 16);
    CHECK(group.size == sizeof(DispatchIndirectArgs));
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a declared access becomes a use at the draw or dispatch that consumes it", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Texture written = kit.texture(Format::RGBA8Unorm, TextureUsage::Storage, 2, 2);
    const Texture read = kit.texture();
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite, 256);
    move(kit.list, written, Layout::Undefined, Layout::ShaderWrite);
    move(kit.list, read, Layout::Undefined, Layout::ShaderRead);
    const size_t before = core::coreOf(kit.list).trackerLog.events.size();
    const std::array<ResourceAccess, 3> accesses{
        ResourceAccess{ .texture = written,
                        .range = { .baseMip = 1, .mipCount = 1, .baseLayer = 0, .layerCount = 1 },
                        .stage = Stage::ComputeShader,
                        .access = Access::ShaderWrite },
        ResourceAccess{ .texture = read, .stage = Stage::ComputeShader, .access = Access::ShaderRead },
        ResourceAccess{ .buffer = buffer, .offset = 64, .size = 32, .stage = Stage::ComputeShader, .access = Access::ShaderWrite }
    };
    kit.list.setPipeline(kit.compute());
    kit.list.declareAccess(accesses);
    CHECK(core::coreOf(kit.list).trackerLog.events.size() == before);
    kit.list.dispatch(1, 1, 1);
    REQUIRE(core::coreOf(kit.list).trackerLog.events.size() == before + 3);
    const core::UseEvent write = useAt(kit.list, before);
    CHECK(write.texture == written);
    CHECK(write.range.baseMip == 1);
    CHECK(write.range.mipCount == 1);
    CHECK(write.stage == Stage::ComputeShader);
    CHECK(write.access == Access::ShaderWrite);
    CHECK(write.layouts == core::layoutSet({ Layout::ShaderWrite }));
    CHECK(write.rule == core::UseRule::Sync0004);
    const core::UseEvent sampled = useAt(kit.list, before + 1);
    CHECK(sampled.texture == read);
    CHECK(sampled.access == Access::ShaderRead);
    CHECK(sampled.layouts == core::layoutSet({ Layout::ShaderRead, Layout::DepthStencilRead, Layout::General, Layout::ShaderWrite }));
    CHECK(sampled.rule == core::UseRule::Sync0004);
    const core::UseEvent bytes = useAt(kit.list, before + 2);
    CHECK(bytes.buffer == buffer);
    CHECK(bytes.offset == 64);
    CHECK(bytes.size == 32);
    CHECK(bytes.layouts == 0);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("an upscale logs mip 0 of layer 0 of each texture it reads and writes", "[core]")
{
#ifndef NDEBUG
    UpscaleKit kit(QueueType::Graphics, UpscaleKit::defaultDesc(), true);
    for (const Texture input : { kit.params.color, kit.params.depth, kit.params.motion })
    {
        move(kit.kit.list, input, Layout::Undefined, Layout::ShaderRead);
    }
    move(kit.kit.list, kit.params.output, Layout::Undefined, Layout::ShaderWrite);
    const size_t before = core::coreOf(kit.kit.list).trackerLog.events.size();
    kit.upscale();
    // The kit's params hold no exposure and no reactive mask: color, depth, motion and the output.
    REQUIRE(core::coreOf(kit.kit.list).trackerLog.events.size() == before + 4);
    const std::array<Texture, 4> textures{ kit.params.color, kit.params.depth, kit.params.motion, kit.params.output };
    for (size_t index = 0; index < textures.size(); ++index)
    {
        const core::UseEvent use = useAt(kit.kit.list, before + index);
        CHECK(use.texture == textures[index]);
        CHECK(use.range.baseMip == 0);
        CHECK(use.range.mipCount == 1);
        CHECK(use.range.baseLayer == 0);
        CHECK(use.range.layerCount == 1);
        CHECK(use.stage == Stage::ComputeShader);
        CHECK(use.rule == core::UseRule::Upscale0016);
        CHECK(use.access == (index == 3 ? Access::ShaderWrite : Access::ShaderRead));
    }
    // The four uses are one command's, distinct from the barrier before them.
    const core::TrackerLog& log = core::coreOf(kit.kit.list).trackerLog;
    REQUIRE(log.commands.size() == log.events.size());
    for (size_t index = before; index < log.events.size(); ++index)
    {
        CHECK(log.commands[index] == log.commands[before]);
    }
    CHECK(log.commands[before] != log.commands[before - 1]);
    kit.kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("every logging entry gives its events one command ordinal, distinct from the command before", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const core::TrackerLog& log = core::coreOf(kit.list).trackerLog;
    std::optional<uint64_t> previous;
    const auto command = [&](const char* name, const auto& record) {
        INFO(name);
        const size_t before = log.events.size();
        record();
        REQUIRE(log.commands.size() == log.events.size());
        REQUIRE(log.events.size() > before);
        const uint64_t ordinal = log.commands[before];
        for (size_t index = before; index < log.events.size(); ++index)
        {
            CHECK(log.commands[index] == ordinal);
        }
        CHECK(previous != ordinal);
        previous = ordinal;
    };
    // Each command uses resources of its own, so the list is also valid at submission.
    const TextureRegion region{ .extent = { 4, 4, 1 } };
    const BufferTextureLayout layout{ .rowPitch = 256 };
    const Texture source = kit.texture();
    const Texture destination = kit.texture();
    const Texture copied = kit.texture();
    const Texture cleared = kit.target();
    const Texture color = kit.target();
    const Buffer indices = kit.buffer(BufferUsage::Index);
    const Buffer args = kit.buffer(BufferUsage::IndirectArgs);
    const Buffer read = kit.buffer(BufferUsage::None);
    const Buffer written = kit.buffer(BufferUsage::ShaderWrite);
    command("barrier", [&] {
        constexpr Access kCopies = Access::CopyRead | Access::CopyWrite;
        const std::array<TextureBarrier, 5> barriers{
            TextureBarrier{ .texture = source, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead, .newLayout = Layout::CopySrc },
            TextureBarrier{ .texture = destination, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite, .newLayout = Layout::CopyDst },
            TextureBarrier{ .texture = copied, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite, .newLayout = Layout::CopyDst },
            TextureBarrier{ .texture = cleared, .dstStage = Stage::Copy, .dstAccess = kCopies, .newLayout = Layout::General },
            TextureBarrier{ .texture = color,
                            .dstStage = Stage::ColorAttachment,
                            .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                            .newLayout = Layout::ColorAttachment },
        };
        kit.list.barrier({}, {}, barriers);
    });
    command("copyBuffer", [&] { kit.list.copyBuffer(kit.buffer(BufferUsage::None), 0, kit.buffer(BufferUsage::None), 0, 256); });
    command("fillBuffer", [&] { kit.list.fillBuffer(kit.buffer(BufferUsage::ShaderWrite), 0, 256, 0); });
    command("copyBufferToTexture", [&] { kit.list.copyBufferToTexture(kit.buffer(BufferUsage::None), layout, destination, region); });
    command("copyTextureToBuffer", [&] { kit.list.copyTextureToBuffer(source, region, kit.buffer(BufferUsage::None), layout); });
    command("copyTexture", [&] { kit.list.copyTexture(source, region, copied, region); });
    command("clearTexture", [&] { kit.list.clearTexture(cleared, {}, {}); });
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = color } };
    command("beginRenderPass", [&] { kit.list.beginRenderPass(test::fullPass(colors)); });
    constexpr std::array<Format, 1> formats = { Format::RGBA8Unorm };
    kit.list.setPipeline(kit.graphics(formats));
    kit.list.setViewport({ .width = static_cast<float>(PassKit::kSize), .height = static_cast<float>(PassKit::kSize) });
    kit.list.setScissor({ 0, 0, PassKit::kSize, PassKit::kSize });
    const std::array<ResourceAccess, 1> fragment{ ResourceAccess{ .buffer = read, .stage = Stage::FragmentShader, .access = Access::ShaderRead } };
    command("draw", [&] {
        kit.list.declareAccess(fragment);
        kit.list.draw(3);
    });
    command("drawIndexed", [&] { kit.list.drawIndexed(indices, IndexType::UInt32, 3); });
    command("drawIndirect", [&] { kit.list.drawIndirect(kit.device.address(args, 0), 1); });
    command("drawIndexedIndirect", [&] { kit.list.drawIndexedIndirect(indices, IndexType::UInt32, kit.device.address(args, 0), 1); });
    kit.list.endRenderPass();
    kit.list.setPipeline(kit.compute());
    const std::array<ResourceAccess, 1> compute{ ResourceAccess{ .buffer = written, .stage = Stage::ComputeShader, .access = Access::ShaderWrite } };
    command("dispatch", [&] {
        kit.list.declareAccess(compute);
        kit.list.dispatch(1, 1, 1);
    });
    command("dispatchIndirect", [&] { kit.list.dispatchIndirect(kit.device.address(args, 0)); });
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("the tracker numbers commands whatever the backend counts as native calls", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Buffer source = kit.buffer(BufferUsage::None);
    const Buffer destination = kit.buffer(BufferUsage::ShaderWrite);
    const core::TrackerLog& log = core::coreOf(kit.list).trackerLog;
    kit.list.copyBuffer(source, 0, destination, 0, 16);
    core::coreOf(kit.list).counts.nativeRecordCalls = 1000;
    kit.list.copyBuffer(source, 16, destination, 16, 16);
    REQUIRE(log.commands.size() == 4);
    CHECK(log.commands[0] == log.commands[1]);
    CHECK(log.commands[2] == log.commands[3]);
    CHECK(log.commands[2] == log.commands[0] + 1);
    kit.finish();
#endif
}
