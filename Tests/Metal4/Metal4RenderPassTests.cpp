//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4RenderPassTests.cpp
/// @brief Tests how Metal 4 records render passes: the lazily opened render encoder, its descriptor, store actions, debug groups and splits.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Metal4Barriers.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4DrawState.h"
#include "Metal4Encoders.h"
#include "Support/Backend.h"
#include "Support/GpuKit.h"
#include "Support/Metal4/Metal4Timing.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// The encoders of `list`.
    const metal4::Encoders& encodersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders;
    }

    //==================================================================================================================
    /// The native state of `list`.
    const metal4::ListState& stateOf(CommandList list)
    {
        return CommandListImpl::of(list).state;
    }

    //==================================================================================================================
    /// The pass state of `list`.
    const metal4::DrawState& drawOf(CommandList list)
    {
        return CommandListImpl::of(list).draw;
    }

    //==================================================================================================================
    /// What `list` has counted so far; fails the case when a scope it recorded holds no timestamp pair, since the pinned
    /// counts include both timestamp writes.
    const core::RecordCounts& countsOf(CommandList list)
    {
        return test::timedCounts(list);
    }

    //==================================================================================================================
    /// The UTF-8 text of `string`, read inside a pool because a getter may autorelease.
    std::string textOf(NS::String* string)
    {
        const metal4::AutoreleasePool pool;
        return metal4::toStdString(string);
    }

    //==================================================================================================================
    /// The label of the render encoder `list` has open, read inside a pool because the getter autoreleases.
    std::string renderLabel(CommandList list)
    {
        MTL4::RenderCommandEncoder* encoder = encodersOf(list).renderEncoder();
        REQUIRE(encoder != nullptr);
        const metal4::AutoreleasePool pool;
        return metal4::toStdString(encoder->label());
    }

    //==================================================================================================================
    /// Color attachment `index` of `list`'s render pass descriptor, read inside a pool because the getters autorelease; the
    /// descriptor keeps it.
    const MTL::RenderPassColorAttachmentDescriptor* colorOf(CommandList list, NS::UInteger index)
    {
        const metal4::AutoreleasePool pool;
        return drawOf(list).descriptor->colorAttachments()->object(index);
    }

    //==================================================================================================================
    /// The depth attachment of `list`'s render pass descriptor, read inside a pool.
    const MTL::RenderPassDepthAttachmentDescriptor* depthOf(CommandList list)
    {
        const metal4::AutoreleasePool pool;
        return drawOf(list).descriptor->depthAttachment();
    }

    //==================================================================================================================
    /// The stencil attachment of `list`'s render pass descriptor, read inside a pool.
    const MTL::RenderPassStencilAttachmentDescriptor* stencilOf(CommandList list)
    {
        const metal4::AutoreleasePool pool;
        return drawOf(list).descriptor->stencilAttachment();
    }

    //==================================================================================================================
    /// True when `attachment` names a texture, read inside a pool.
    bool holdsTexture(const MTL::RenderPassAttachmentDescriptor* attachment)
    {
        const metal4::AutoreleasePool pool;
        return attachment->texture() != nullptr;
    }

    //==================================================================================================================
    /// The texture `attachment` names, read inside a pool.
    const MTL::Texture* textureOf(const MTL::RenderPassAttachmentDescriptor* attachment)
    {
        const metal4::AutoreleasePool pool;
        return attachment->texture();
    }

    //==================================================================================================================
    /// The resolve texture of color attachment `index` of `list`'s render pass descriptor, read inside a pool.
    const MTL::Texture* resolveOf(CommandList list, NS::UInteger index)
    {
        const metal4::AutoreleasePool pool;
        return drawOf(list).descriptor->colorAttachments()->object(index)->resolveTexture();
    }

    //==================================================================================================================
    /// True when `attachment` names a resolve texture, read inside a pool.
    bool holdsResolve(const MTL::RenderPassAttachmentDescriptor* attachment)
    {
        const metal4::AutoreleasePool pool;
        return attachment->resolveTexture() != nullptr;
    }

    //==================================================================================================================
    /// Creates a 16-texel square 2D texture of `format` with `usage` and `samples`.
    Texture makeTexture(Device device, Format format, TextureUsage usage = TextureUsage::RenderTarget, uint32_t samples = 1)
    {
        return *device.createTexture(
            TextureDesc{ .type = TextureType::Tex2D, .format = format, .extent = { 16, 16, 1 }, .samples = samples, .usage = usage, .label = "test.target" }
        );
    }

    //==================================================================================================================
    /// A pass over `colors` and `depth` that renders to the whole 16-texel square.
    RenderPassDesc passOf(std::span<const ColorAttachment> colors, const DepthAttachment& depth = {}, std::string_view label = {})
    {
        return RenderPassDesc{ .colors = colors, .depth = depth, .area = { 0, 0, 16, 16 }, .label = label };
    }

    //==================================================================================================================
    /// Ends `list`, submits it to the graphics queue, waits for it on the host and ends the frame.
    void finish(Device device, CommandList list)
    {
        list.end();
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(QueueType::Graphics).submit(std::array{ list }, {}, signal);
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
    }

    //==================================================================================================================
    /// The native calls `endRenderPass` records on `list`.
    uint64_t endPassCalls(CommandList list)
    {
        const uint64_t before = countsOf(list).nativeRecordCalls;
        list.endRenderPass();
        return countsOf(list).nativeRecordCalls - before;
    }
} // namespace

//======================================================================================================================
TEST_CASE("beginRenderPass closes the compute encoder, pushes the pass's debug group and opens no encoder", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    list.copyBuffer(a, 0, b, 0, 256);
    REQUIRE(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    REQUIRE(countsOf(list).nativeRecordCalls == 2 + 2 + label + 1);
    const std::array colors{ ColorAttachment{ .texture = target } };
    list.beginRenderPass(passOf(colors, {}, "test.pass"));
    // The compute encoder ends (1), and the pass's group (1) goes on the command buffer; its begin timestamp waits for the
    // pass's first encoder.
    CHECK(countsOf(list).nativeRecordCalls == 5 + label + 2);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    REQUIRE(stateOf(list).scopeLabels.size() == 1);
    CHECK(textOf(stateOf(list).scopeLabels.back()) == "test.pass");
    CHECK(drawOf(list).passOpen);
    CHECK_FALSE(drawOf(list).resuming);
    REQUIRE(drawOf(list).colors.size() == 1);
    CHECK(drawOf(list).colors[0].texture == core::coreOf(list).device->pool<Texture>().native(target));
    list.endRenderPass();
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a pass with no command opens its render encoder at endRenderPass", "[metal4]")
{
    Device device = *createDevice();
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(colors));
    CHECK(countsOf(list).nativeRecordCalls == 2 + 1);
    CHECK(countsOf(list).encodersOpened == 0);
    // The factory, the label and the table, the begin and end timestamps on the encoder, and the pass's group popped on the
    // command buffer; the encoder stays open, running no further command, until the next encoder or `end`.
    list.endRenderPass();
    CHECK(countsOf(list).nativeRecordCalls == 3 + 5 + label);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 0);
    CHECK(encodersOf(list).lingering());
    CHECK(encodersOf(list).renderEncoder() == nullptr);
    CHECK(stateOf(list).scopeLabels.empty());
    CHECK_FALSE(drawOf(list).passOpen);
    // The list ends right after the pass.
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Store attachment costs no store-action call; DontCare and resolve attachments set theirs at the end", "[metal4]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    // An empty pass's encoder records its factory, label when encoders carry one, table and the pass's two timestamps, then the pass's group pops; it
    // ends at the next beginRenderPass.
    const uint64_t base = 5 + test::encoderLabel(list);
    const MTL::RenderPassColorAttachmentDescriptor* color = nullptr;
    SECTION("color attachments")
    {
        const std::array stored{ ColorAttachment{ .texture = makeTexture(device, Format::RGBA8Unorm), .load = LoadOp::Clear } };
        list.beginRenderPass(passOf(stored));
        CHECK(endPassCalls(list) == base);
        color = colorOf(list, 0);
        CHECK(color->storeAction() == MTL::StoreActionStore);
        const std::array discarded{ ColorAttachment{ .texture = makeTexture(device, Format::RGBA8Unorm), .store = StoreOp::DontCare } };
        list.beginRenderPass(passOf(discarded));
        CHECK(endPassCalls(list) == base + 1);
        CHECK(color->storeAction() == MTL::StoreActionUnknown);
        for (const StoreOp store : { StoreOp::Store, StoreOp::DontCare })
        {
            const std::array resolved{ ColorAttachment{ .texture = makeTexture(device, Format::RGBA8Unorm, TextureUsage::RenderTarget, 4),
                                                        .load = LoadOp::Clear,
                                                        .store = store,
                                                        .resolve = makeTexture(device, Format::RGBA8Unorm) } };
            list.beginRenderPass(passOf(resolved));
            CHECK(endPassCalls(list) == base + 1);
            CHECK(color->storeAction() == MTL::StoreActionUnknown);
            CHECK(holdsResolve(color));
        }
        // Eight attachments, two of which discard.
        std::array<ColorAttachment, 8> eight{};
        for (size_t i = 0; i < eight.size(); ++i)
        {
            eight[i] = ColorAttachment{ .texture = makeTexture(device, Format::R8Unorm), .store = i % 4 == 1 ? StoreOp::DontCare : StoreOp::Store };
        }
        list.beginRenderPass(passOf(eight));
        CHECK(endPassCalls(list) == base + 2);
    }
    SECTION("depth and stencil attachments")
    {
        list.beginRenderPass(
            passOf({}, DepthAttachment{ .texture = makeTexture(device, Format::D32Float, TextureUsage::DepthStencil), .load = LoadOp::Clear })
        );
        CHECK(endPassCalls(list) == base);
        CHECK(depthOf(list)->storeAction() == MTL::StoreActionStore);
        list.beginRenderPass(
            passOf({}, DepthAttachment{ .texture = makeTexture(device, Format::D32Float, TextureUsage::DepthStencil), .store = StoreOp::DontCare })
        );
        CHECK(endPassCalls(list) == base + 1);
        // A format without stencil ignores the stencil fields, whose defaults discard.
        CHECK_FALSE(holdsTexture(stencilOf(list)));
        // The stencil operations default to DontCare, so a depth-stencil pass that stores depth sets the stencil's alone.
        list.beginRenderPass(
            passOf({}, DepthAttachment{ .texture = makeTexture(device, Format::D32FloatS8, TextureUsage::DepthStencil), .load = LoadOp::Clear })
        );
        CHECK(endPassCalls(list) == base + 1);
        // Depth discarded and stencil stored.
        list.beginRenderPass(passOf(
            {},
            DepthAttachment{ .texture = makeTexture(device, Format::D32FloatS8, TextureUsage::DepthStencil),
                             .load = LoadOp::Clear,
                             .store = StoreOp::DontCare,
                             .stencilLoad = LoadOp::Clear,
                             .stencilStore = StoreOp::Store }
        ));
        CHECK(endPassCalls(list) == base + 1);
        CHECK(depthOf(list)->storeAction() == MTL::StoreActionUnknown);
        CHECK(stencilOf(list)->storeAction() == MTL::StoreActionStore);
        list.beginRenderPass(passOf(
            {},
            DepthAttachment{
                .texture = makeTexture(device, Format::D32FloatS8, TextureUsage::DepthStencil), .store = StoreOp::DontCare, .stencilStore = StoreOp::DontCare }
        ));
        CHECK(endPassCalls(list) == base + 2);
    }
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the render encoder is labelled after the pass, or after the innermost scope open inside it", "[metal4]")
{
    Device device = *createDevice();
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    const std::array colors{ ColorAttachment{ .texture = target } };
    list.beginScope("test.outer");
    list.beginRenderPass(passOf(colors, {}, "test.pass"));
    // The scope opens the encoder before its own group goes on it.
    list.beginScope("test.a");
    CHECK(renderLabel(list) == std::string(label != 0 ? "test.pass" : ""));
    metal4::splitPass(CommandListImpl::of(list));
    list.beginScope("test.b");
    CHECK(renderLabel(list) == std::string(label != 0 ? "test.a" : ""));
    list.endScope();
    list.endScope();
    list.endRenderPass();
    // An unnamed pass takes the default label.
    list.beginRenderPass(passOf(colors));
    list.beginScope({});
    CHECK(renderLabel(list) == std::string(label != 0 ? "rojorhi.pass.unnamed" : ""));
    list.endScope();
    list.endRenderPass();
    list.endScope();
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a scope inside a pass opens the render encoder and its debug group goes on the encoder", "[metal4]")
{
    Device device = *createDevice();
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(colors, {}, "test.pass"));
    REQUIRE(countsOf(list).nativeRecordCalls == 3);
    // The factory, the label and the table, the pass's begin timestamp, then the group pushed on the encoder and the scope's
    // begin timestamp.
    list.beginScope("test.inner");
    CHECK(countsOf(list).nativeRecordCalls == 3 + 5 + label);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Render);
    CHECK(drawOf(list).passGroups == 1);
    REQUIRE(stateOf(list).scopeLabels.size() == 2);
    CHECK(textOf(stateOf(list).scopeLabels.back()) == "test.inner");
    // The end timestamp, then the group pops from the encoder, which stays open.
    list.endScope();
    CHECK(countsOf(list).nativeRecordCalls == 8 + label + 2);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Render);
    CHECK(drawOf(list).passGroups == 0);
    CHECK(stateOf(list).scopeLabels.size() == 1);
    // endRenderPass finds the encoder open: the pass's end timestamp, endEncoding and its group.
    list.endRenderPass();
    CHECK(countsOf(list).nativeRecordCalls == 10 + label + 2);
    CHECK(countsOf(list).encodersOpened == 1);
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a split stores, ends and resumes with Load, re-pushing the open in-pass groups", "[metal4]")
{
    Device device = *createDevice();
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear, .store = StoreOp::DontCare } };
    list.beginRenderPass(passOf(colors, {}, "test.pass"));
    list.beginScope("test.a");
    list.beginScope("test.b");
    REQUIRE(countsOf(list).nativeRecordCalls == 3 + 5 + label + 2);
    const MTL::RenderPassColorAttachmentDescriptor* color = colorOf(list, 0);
    CHECK(color->loadAction() == MTL::LoadActionClear);
    CHECK(color->storeAction() == MTL::StoreActionUnknown);
    // Both groups pop from the encoder, the attachment stores and the encoder ends.
    metal4::splitPass(CommandListImpl::of(list));
    CHECK(countsOf(list).nativeRecordCalls == 10 + label + 2 + 1 + 1);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    CHECK(drawOf(list).resuming);
    CHECK(drawOf(list).passGroups == 2);
    // Nothing is open, so a second split does nothing.
    metal4::splitPass(CommandListImpl::of(list));
    CHECK(countsOf(list).nativeRecordCalls == 14 + label);
    // The scope boundary resumes the pass: the factory, the label and the table, both groups again, then the end timestamp
    // and the pop. The split wrote no timestamp.
    list.endScope();
    CHECK(countsOf(list).nativeRecordCalls == 14 + label + 2 + label + 2 + 2);
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(color->loadAction() == MTL::LoadActionLoad);
    CHECK(renderLabel(list) == std::string(label != 0 ? "test.b" : ""));
    list.endScope();
    // The outer scope's end timestamp and pop; the caller's DontCare at the end, the pass's end timestamp, endEncoding and
    // its group.
    list.endRenderPass();
    CHECK(countsOf(list).nativeRecordCalls == 20 + 2 * label + 2 + 1 + 1 + 1);
    CHECK_FALSE(drawOf(list).resuming);
    // The next pass loads as its caller asks.
    list.beginRenderPass(passOf(colors));
    list.endRenderPass();
    CHECK(color->loadAction() == MTL::LoadActionClear);
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a barrier pending before the pass is emitted by the render encoder for the render stages", "[metal4]")
{
    Device device = *createDevice();
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.barrier(
        std::array{ GlobalBarrier{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::All, .dstAccess = Access::CopyWrite } }
    );
    const metal4::BarrierState& barriers = encodersOf(list).barriers();
    REQUIRE(barriers.pendingDestination == (metal4::kComputeStages | metal4::kRenderStages));
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(colors));
    CHECK(countsOf(list).nativeBarriers == 0);
    // The encoder's opening barrier holds back every render stage.
    list.beginScope("test.inner");
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(barriers.openDestination == metal4::kRenderStages);
    CHECK(barriers.pendingDestination == metal4::kComputeStages);
    list.endScope();
    list.endRenderPass();
    // The encoder ran only its load and store actions, so the stages in which it ran nothing are owed again.
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(barriers.pendingDestination == (metal4::kComputeStages | (metal4::kRenderStages & ~MTL::StageFragment)));
    // The pass's encoder waited for every source and carries the rest at end; the All destination names IndirectArgs, so
    // one barrier holds back every stage left, the dispatch stage included.
    list.end();
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(countsOf(list).encodersOpened == 1);
    const Fence fence = *device.createFence(0, "test.done");
    device.queue(QueueType::Graphics).submit(std::array{ list }, {}, std::array{ FenceValue{ fence, 1 } });
    CHECK(device.wait({ fence, 1 }, UINT64_MAX));
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a second pass reuses the list's descriptor and clears attachments the first had", "[metal4]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    CHECK(drawOf(list).descriptor == nullptr);
    const std::array three{ ColorAttachment{ .texture = makeTexture(device, Format::RGBA8Unorm, TextureUsage::RenderTarget, 4),
                                             .store = StoreOp::DontCare,
                                             .resolve = makeTexture(device, Format::RGBA8Unorm) },
                            ColorAttachment{ .texture = makeTexture(device, Format::RGBA8Unorm, TextureUsage::RenderTarget, 4) },
                            ColorAttachment{ .texture = makeTexture(device, Format::RGBA8Unorm, TextureUsage::RenderTarget, 4) } };
    list.beginRenderPass(passOf(three, DepthAttachment{ .texture = makeTexture(device, Format::D32FloatS8, TextureUsage::DepthStencil, 4) }));
    list.endRenderPass();
    const MTL4::RenderPassDescriptor* descriptor = drawOf(list).descriptor;
    REQUIRE(descriptor != nullptr);
    CHECK(drawOf(list).descriptorColors == 3);
    CHECK(holdsTexture(colorOf(list, 2)));
    CHECK(holdsTexture(stencilOf(list)));
    const std::array one{ ColorAttachment{ .texture = makeTexture(device, Format::RGBA8Unorm) } };
    list.beginRenderPass(passOf(one));
    list.endRenderPass();
    CHECK(drawOf(list).descriptor == descriptor);
    CHECK(drawOf(list).descriptorColors == 1);
    CHECK(holdsTexture(colorOf(list, 0)));
    CHECK_FALSE(holdsResolve(colorOf(list, 0)));
    CHECK_FALSE(holdsTexture(colorOf(list, 1)));
    CHECK_FALSE(holdsTexture(colorOf(list, 2)));
    CHECK_FALSE(holdsTexture(depthOf(list)));
    CHECK_FALSE(holdsTexture(stencilOf(list)));
    finish(device, list);
    // A list acquired again forgets its pass and keeps its descriptor.
    const uint32_t slots = core::coreOf(device).framesInFlight();
    for (uint32_t frame = 1; frame < slots; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    device.beginFrame();
    CommandList again = device.acquireCommandList(QueueType::Graphics, "test.again");
    REQUIRE(&CommandListImpl::of(again) == &CommandListImpl::of(list));
    CHECK(drawOf(again).descriptor == descriptor);
    CHECK_FALSE(drawOf(again).passOpen);
    CHECK(drawOf(again).colors.empty());
    finish(device, again);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a split keeps the attachment's cleared and drawn contents across the resumed encoder", "[metal4]")
{
    test::GpuKit kit;
    const ShaderLibrary library = *kit.device.loadShaderLibrary(test::standardLibrary(), "test.standard");
    const ClearColor clear{ 1.0f, 0.0f, 0.2f, 0.6f };
    const std::array<std::byte, 4> texel{ std::byte{ 255 }, std::byte{ 0 }, std::byte{ 51 }, std::byte{ 153 } };
    const TextureDesc desc{ .type = TextureType::Tex2D,
                            .format = Format::RGBA8Unorm,
                            .extent = { 16, 16, 1 },
                            .usage = TextureUsage::RenderTarget | TextureUsage::Sampled,
                            .label = "test.target" };
    const std::vector<std::byte> initial = test::pattern(16 * 16 * 4, 7);
    const std::array data{ TextureData{ initial, 16 * 4, 0 } };
    const Texture target = *kit.device.createTexture(desc, data);
    CommandList list = kit.list(QueueType::Graphics, "test.split");
    std::array<ColorAttachment, 1> colors{};
    Texture read = target;
    uint32_t samples = 1;
    SECTION("a stored attachment")
    {
        test::transition(list, target, Layout::ShaderRead, Layout::ColorAttachment);
        colors[0] = ColorAttachment{ .texture = target, .load = LoadOp::Clear, .clear = clear };
    }
    SECTION("a multisample attachment resolved at the end")
    {
        const Texture multisample = *kit.device.createTexture(
            TextureDesc{ .type = TextureType::Tex2D,
                         .format = Format::RGBA8Unorm,
                         .extent = { 16, 16, 1 },
                         .samples = 4,
                         .usage = TextureUsage::RenderTarget,
                         .label = "test.ms" }
        );
        test::transition(list, multisample, Layout::Undefined, Layout::ColorAttachment);
        test::transition(list, target, Layout::ShaderRead, Layout::ColorAttachment);
        colors[0] = ColorAttachment{ .texture = multisample, .load = LoadOp::Clear, .store = StoreOp::DontCare, .clear = clear, .resolve = target };
        samples = 4;
    }
    // A fullscreen triangle that writes alpha alone, 1.0 exactly, so a resolve of covered texels is exact too: over the left
    // half before the split, over the lower half after it.
    const std::array targets{ ColorTarget{ .format = Format::RGBA8Unorm, .writeMask = ColorWriteMask::A } };
    const Pipeline alpha = *kit.device.createPipeline(
        GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .colors = targets, .samples = samples, .label = "test.alpha" }
    );
    list.beginRenderPass(passOf(colors));
    list.setPipeline(alpha);
    list.setViewport(Viewport{ .width = 16.0f, .height = 16.0f });
    list.setScissor(Rect2D{ 0, 0, 8, 16 });
    list.beginScope("test.scope");
    list.draw(3);
    metal4::splitPass(CommandListImpl::of(list));
    list.setScissor(Rect2D{ 0, 8, 16, 8 });
    list.draw(3);
    list.endScope();
    list.endRenderPass();
    CHECK(countsOf(list).encodersOpened == 2);
    test::transition(list, read, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    std::vector<std::byte> expected;
    for (uint32_t y = 0; y < 16; ++y)
    {
        for (uint32_t x = 0; x < 16; ++x)
        {
            expected.insert(expected.end(), texel.begin(), texel.end());
            if (x < 8 || y >= 8)
            {
                expected.back() = std::byte{ 255 };
            }
        }
    }
    CHECK(kit.readTexture(read, TextureRegion{ .extent = { 16, 16, 1 } }, Format::RGBA8Unorm) == expected);
}

//======================================================================================================================
TEST_CASE("the descriptor takes each pass's attachment values though it is set only where they change", "[metal4]")
{
    Device device = *createDevice();
    const Texture first = makeTexture(device, Format::RGBA8Unorm);
    const Texture second = makeTexture(device, Format::RGBA8Unorm);
    const Texture depth = makeTexture(device, Format::D32Float, TextureUsage::DepthStencil);
    const auto* firstNative = static_cast<const MTL::Texture*>(core::coreOf(device).pool<Texture>().native(first));
    const auto* depthNative = static_cast<const MTL::Texture*>(core::coreOf(device).pool<Texture>().native(depth));
    const std::array red{ ColorAttachment{ .texture = first, .load = LoadOp::Clear, .clear = { 1.0f, 0.0f, 0.0f, 1.0f } } };
    const std::array loaded{ ColorAttachment{ .texture = first, .load = LoadOp::Load, .clear = { 1.0f, 0.0f, 0.0f, 1.0f } } };
    const std::array green{ ColorAttachment{ .texture = second, .load = LoadOp::Clear, .store = StoreOp::DontCare, .clear = { 0.0f, 1.0f, 0.0f, 1.0f } } };
    const DepthAttachment cleared{ .texture = depth, .load = LoadOp::Clear, .clearDepth = 0.5f };
    const Texture depthStencil = makeTexture(device, Format::D32FloatS8, TextureUsage::DepthStencil);
    const auto* depthStencilNative = static_cast<const MTL::Texture*>(core::coreOf(device).pool<Texture>().native(depthStencil));
    const DepthAttachment zeroed{ .texture = depthStencil, .load = LoadOp::Clear, .clearDepth = 0.0f, .stencilLoad = LoadOp::Clear };
    const DepthAttachment stenciled{ .texture = depthStencil, .load = LoadOp::Clear, .clearDepth = -0.0f, .stencilLoad = LoadOp::Clear, .clearStencil = 7 };
    const Texture multiA = makeTexture(device, Format::RGBA8Unorm, TextureUsage::RenderTarget, 4);
    const Texture multiB = makeTexture(device, Format::RGBA8Unorm, TextureUsage::RenderTarget, 4);
    const Texture resolved = makeTexture(device, Format::RGBA8Unorm);
    const auto* resolvedNative = static_cast<const MTL::Texture*>(core::coreOf(device).pool<Texture>().native(resolved));
    const std::array two{ ColorAttachment{ .texture = multiA, .load = LoadOp::Clear }, ColorAttachment{ .texture = multiB, .load = LoadOp::Clear } };
    const std::array resolving{ ColorAttachment{ .texture = multiA, .load = LoadOp::Clear, .store = StoreOp::DontCare, .resolve = resolved } };
    const uint32_t slots = core::coreOf(device).framesInFlight();
    for (int recording = 0; recording < 2; ++recording)
    {
        CAPTURE(recording);
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        // Each pass opens its encoder at endRenderPass, from the values it configured.
        list.beginRenderPass(passOf(red, cleared));
        list.endRenderPass();
        CHECK(textureOf(colorOf(list, 0)) == firstNative);
        CHECK(colorOf(list, 0)->loadAction() == MTL::LoadActionClear);
        CHECK(colorOf(list, 0)->clearColor().red == 1.0);
        CHECK(colorOf(list, 0)->storeAction() == MTL::StoreActionStore);
        CHECK(textureOf(depthOf(list)) == depthNative);
        CHECK(depthOf(list)->clearDepth() == 0.5);
        CHECK(drawOf(list).colorHeld[0].texture == firstNative);
        // Only the load action differs from what the descriptor holds, and the pass has no depth attachment.
        list.beginRenderPass(passOf(loaded));
        list.endRenderPass();
        CHECK(textureOf(colorOf(list, 0)) == firstNative);
        CHECK(colorOf(list, 0)->loadAction() == MTL::LoadActionLoad);
        CHECK(colorOf(list, 0)->clearColor().red == 1.0);
        CHECK_FALSE(holdsTexture(depthOf(list)));
        CHECK(drawOf(list).depthHeld.texture == nullptr);
        // Another texture, clear color and store action.
        list.beginRenderPass(passOf(green));
        list.endRenderPass();
        CHECK(textureOf(colorOf(list, 0)) != firstNative);
        CHECK(colorOf(list, 0)->loadAction() == MTL::LoadActionClear);
        CHECK(colorOf(list, 0)->clearColor().red == 0.0);
        CHECK(colorOf(list, 0)->clearColor().green == 1.0);
        CHECK(colorOf(list, 0)->storeAction() == MTL::StoreActionUnknown);
        // A depth-stencil texture takes both attachments, and a negative zero after a zero is a clear depth of its own.
        list.beginRenderPass(passOf(red, zeroed));
        list.endRenderPass();
        CHECK_FALSE(std::signbit(depthOf(list)->clearDepth()));
        list.beginRenderPass(passOf(red, stenciled));
        list.endRenderPass();
        CHECK(textureOf(depthOf(list)) == depthStencilNative);
        CHECK(std::signbit(depthOf(list)->clearDepth()));
        CHECK(textureOf(stencilOf(list)) == depthStencilNative);
        CHECK(stencilOf(list)->clearStencil() == 7);
        CHECK(stencilOf(list)->loadAction() == MTL::LoadActionClear);
        // A narrower pass after a wider one nulls the slot it no longer names, and the depth and stencil it lacks.
        list.beginRenderPass(passOf(two));
        list.endRenderPass();
        CHECK(holdsTexture(colorOf(list, 1)));
        list.beginRenderPass(passOf(resolving));
        list.endRenderPass();
        CHECK(holdsTexture(colorOf(list, 0)));
        CHECK(resolveOf(list, 0) == resolvedNative);
        CHECK_FALSE(holdsTexture(colorOf(list, 1)));
        CHECK(drawOf(list).colorHeld[1].texture == nullptr);
        CHECK_FALSE(holdsTexture(depthOf(list)));
        CHECK_FALSE(holdsTexture(stencilOf(list)));
        // end lets go of the textures, and the descriptor's record of them follows.
        list.end();
        CHECK_FALSE(holdsTexture(colorOf(list, 0)));
        CHECK_FALSE(holdsResolve(colorOf(list, 0)));
        CHECK(drawOf(list).colorHeld[0].resolve == nullptr);
        CHECK_FALSE(holdsTexture(depthOf(list)));
        CHECK_FALSE(holdsTexture(stencilOf(list)));
        CHECK(drawOf(list).colorHeld[0].texture == nullptr);
        CHECK(drawOf(list).depthHeld.texture == nullptr);
        CHECK(drawOf(list).stencilHeld.texture == nullptr);
        const Fence fence = *device.createFence(0, "test.done");
        device.queue(QueueType::Graphics).submit(std::array{ list }, {}, std::array{ FenceValue{ fence, 1 } });
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
        // The next recording takes the same list from the same slot.
        for (uint32_t frame = 1; frame < slots; ++frame)
        {
            device.beginFrame();
            device.endFrame();
        }
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("end empties the list's descriptor so it keeps no texture alive in the pool", "[metal4]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::array colors{ ColorAttachment{ .texture = makeTexture(device, Format::RGBA8Unorm, TextureUsage::RenderTarget, 4),
                                              .store = StoreOp::DontCare,
                                              .resolve = makeTexture(device, Format::RGBA8Unorm) },
                             ColorAttachment{ .texture = makeTexture(device, Format::RGBA8Unorm, TextureUsage::RenderTarget, 4) } };
    list.beginRenderPass(passOf(colors, DepthAttachment{ .texture = makeTexture(device, Format::D32FloatS8, TextureUsage::DepthStencil, 4) }));
    list.endRenderPass();
    REQUIRE(holdsTexture(colorOf(list, 1)));
    REQUIRE(holdsResolve(colorOf(list, 0)));
    REQUIRE(holdsTexture(depthOf(list)));
    const uint64_t calls = countsOf(list).nativeRecordCalls;
    list.end();
    CHECK(drawOf(list).descriptor != nullptr);
    CHECK(drawOf(list).descriptorColors == 0);
    CHECK_FALSE(holdsTexture(colorOf(list, 0)));
    CHECK_FALSE(holdsResolve(colorOf(list, 0)));
    CHECK_FALSE(holdsTexture(colorOf(list, 1)));
    CHECK_FALSE(holdsTexture(depthOf(list)));
    CHECK_FALSE(holdsTexture(stencilOf(list)));
    // Descriptor setters are no message to a command buffer, encoder or table: end counts only the pass's endEncoding and
    // endCommandBuffer.
    CHECK(countsOf(list).nativeRecordCalls == calls + 2);
    const Fence fence = *device.createFence(0, "test.done");
    device.queue(QueueType::Graphics).submit(std::array{ list }, {}, std::array{ FenceValue{ fence, 1 } });
    CHECK(device.wait({ fence, 1 }, UINT64_MAX));
    device.endFrame();
    destroyDevice(device);
}
