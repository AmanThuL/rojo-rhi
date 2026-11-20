//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4GraphicsStateTests.cpp
/// @brief Tests how Metal 4 records graphics state and direct draws: the prefix, the filtered encoder state, the root and compute binding.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Metal4Barriers.h"
#include "Metal4Device.h"
#include "Metal4DrawState.h"
#include "Metal4Encoders.h"
#include "Metal4PipelineState.h"
#include "Support/Backend.h"
#include "Support/GpuKit.h"
#include "Support/Metal4/Metal4Timing.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <vector>

using namespace rojoRHI;

namespace {
    constexpr Viewport kViewport{ .width = 16.0f, .height = 16.0f };
    constexpr Rect2D kScissor{ 0, 0, 16, 16 };

    // What an encoder calls when it opens with no barrier owed: the factory and the label when `list`'s encoders carry one.
    uint64_t openCalls(CommandList list)
    {
        return 1 + test::encoderLabel(list);
    }

    // The list's table, which the lazy compute encoder binds when it opens, and a pass's render encoder before its first
    // draw.
    constexpr uint64_t kTable = 1;

    // The begin timestamp a pass's first render encoder writes right after it opens.
    constexpr uint64_t kPassBegin = 1;

    const metal4::Encoders& encodersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders;
    }

    const metal4::DrawState& drawOf(CommandList list)
    {
        return CommandListImpl::of(list).draw;
    }

    // Fails the case when a scope `list` recorded holds no timestamp pair, since the pinned counts include both
    // timestamp writes.
    const core::RecordCounts& countsOf(CommandList list)
    {
        return test::timedCounts(list);
    }

    uint64_t callsOf(CommandList list)
    {
        return countsOf(list).nativeRecordCalls;
    }

    const metal4::PipelineState* stateOf(CommandList list, Pipeline pipeline)
    {
        return static_cast<const metal4::PipelineState*>(core::coreOf(list).device->pool<Pipeline>().native(pipeline));
    }

    uint64_t gpuAddressOf(Addr address)
    {
        return uint64_t{ address.lo } | uint64_t{ address.hi } << 32;
    }

    ShaderLibrary standard(Device device)
    {
        const std::vector<std::byte> blob = test::standardLibrary();
        return *device.loadShaderLibrary(blob, "test.library");
    }

    Pipeline vertexPipeline(
        Device device, ShaderLibrary library, const RasterState& raster = {}, const DepthStencilState& depthStencil = {}, Format depth = Format::Undefined
    )
    {
        const std::array colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
        return *device.createPipeline(
            GraphicsPipelineDesc{ .vertex = { library, "vsMain" },
                                  .fragment = { library, "fsMain" },
                                  .raster = raster,
                                  .depthStencil = depthStencil,
                                  .colors = colors,
                                  .depthFormat = depth,
                                  .label = "test.pipeline" }
        );
    }

    Texture makeTexture(Device device, Format format, TextureUsage usage = TextureUsage::RenderTarget)
    {
        return *device.createTexture(
            TextureDesc{ .type = TextureType::Tex2D, .format = format, .extent = { 16, 16, 1 }, .usage = usage, .label = "test.target" }
        );
    }

    RenderPassDesc passOf(std::span<const ColorAttachment> colors, const DepthAttachment& depth = {})
    {
        return RenderPassDesc{ .colors = colors, .depth = depth, .area = { 0, 0, 16, 16 }, .label = "test.pass" };
    }

    void finish(Device device, CommandList list)
    {
        list.end();
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(QueueType::Graphics).submit(std::array{ list }, {}, signal);
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
    }

    Addr uploadRoot(CommandList list)
    {
        const std::array<std::byte, 256> zeroes{};
        return list.uploadFrameData(zeroes, 256);
    }

    std::vector<metal4::PrefixKind> prefixKinds(CommandList list)
    {
        std::vector<metal4::PrefixKind> kinds;
        for (const metal4::PrefixCommand& command : drawOf(list).prefix)
        {
            kinds.push_back(command.kind);
        }
        return kinds;
    }
} // namespace

TEST_CASE("state recorded before the first draw waits in the prefix and replays in order at the open", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true, .debugMarkers = true });
    const ShaderLibrary library = standard(device);
    const Pipeline a = vertexPipeline(device, library);
    const Pipeline b = vertexPipeline(device, library);
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const Addr root = uploadRoot(list);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(colors));
    const uint64_t base = callsOf(list);
    list.setPipeline(a);
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    list.setRoot(root);
    list.setStencilReference(3);
    list.setBlendConstants(1.0f, 0.0f, 0.0f, 1.0f);
    // Nothing reaches Metal until the render encoder opens.
    CHECK(callsOf(list) == base);
    CHECK(countsOf(list).encodersOpened == 0);
    using Kind = metal4::PrefixKind;
    CHECK(prefixKinds(list) == std::vector{ Kind::Pipeline, Kind::Viewport, Kind::Scissor, Kind::StencilReference, Kind::BlendColor });
    // The root joins no prefix: the draw writes it. The open and the pass's begin timestamp, then each entry in order: the
    // pipeline with its counter-clockwise winding (it has a fragment entry; the rest of its raster state is Metal's default),
    // the viewport, the scissor, the stencil reference and the blend color; then the table, the table's root and the draw.
    list.draw(3);
    CHECK(callsOf(list) == base + openCalls(list) + kPassBegin + 7 + kTable + 1);
    CHECK(drawOf(list).prefix.empty());
    const metal4::RenderHeld& held = drawOf(list).held;
    CHECK(held.pipeline == stateOf(list, a));
    CHECK(held.viewportKnown);
    CHECK(held.scissorKnown);
    CHECK(held.stencilReference == 3);
    CHECK(held.blendColor == std::array{ 1.0f, 0.0f, 0.0f, 1.0f });
    CHECK(drawOf(list).tableRoot == gpuAddressOf(root));
    list.endRenderPass();

    // A pass that binds two pipelines and draws with neither: the later entry of a kind replaces the earlier, so only the last
    // pipeline and viewport reach the encoder, at endRenderPass.
    list.beginRenderPass(passOf(colors));
    list.setPipeline(a);
    list.setViewport(kViewport);
    list.setPipeline(b);
    list.setViewport(Viewport{ .width = 8.0f, .height = 8.0f });
    CHECK(prefixKinds(list) == std::vector{ Kind::Pipeline, Kind::Viewport });
    const uint64_t before = callsOf(list);
    // The open and the pass's begin timestamp, the pipeline with its winding and the viewport, then the pass's end timestamp
    // and its group; the encoder draws nothing, so it binds no table, and stays open until the list ends.
    list.endRenderPass();
    CHECK(callsOf(list) == before + openCalls(list) + kPassBegin + 3 + 2);
    finish(device, list);
    destroyDevice(device);
}

TEST_CASE("setPipeline sets only the state that differs from the encoder's", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const ShaderLibrary library = standard(device);
    // C3's pipelines: A culls nothing and tests no depth; B culls back faces and tests depth Greater with writes.
    const Pipeline a = vertexPipeline(device, library, RasterState{ .cull = CullMode::None }, DepthStencilState{}, Format::D32Float);
    const Pipeline b = vertexPipeline(
        device,
        library,
        RasterState{ .cull = CullMode::Back },
        DepthStencilState{ .depthTest = true, .depthWrite = true, .depthCompare = CompareOp::Greater },
        Format::D32Float
    );
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    const Texture depth = makeTexture(device, Format::D32Float, TextureUsage::DepthStencil);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(colors, DepthAttachment{ .texture = depth, .load = LoadOp::Clear }));
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    list.setPipeline(a);
    list.draw(3);
    const metal4::RenderHeld& held = drawOf(list).held;
    CHECK(held.depthStencil == nullptr);
    // A has a fragment entry, so its counter-clockwise winding is set once per encoder, with the pipeline.
    CHECK(held.winding == MTL::WindingCounterClockwise);
    uint64_t before = callsOf(list);
    // The pipeline, the depth-stencil state and the cull mode; B's winding is the one the encoder holds. Then the draw.
    list.setPipeline(b);
    list.draw(3);
    CHECK(callsOf(list) - before == 4);
    CHECK(held.winding == MTL::WindingCounterClockwise);
    CHECK(held.depthStencil == stateOf(list, b)->depthStencil);
    // Each later switch costs C3's four calls: the two pipelines share their winding.
    for (int i = 0; i < 4; ++i)
    {
        before = callsOf(list);
        list.setPipeline(i % 2 == 0 ? a : b);
        list.draw(3);
        CHECK(callsOf(list) - before == 4);
    }
    // The same pipeline again sends nothing.
    before = callsOf(list);
    list.setPipeline(b);
    CHECK(callsOf(list) == before);
    list.endRenderPass();
    finish(device, list);
    destroyDevice(device);
}

TEST_CASE("setPipeline reads a pipeline bound again in the recording from the list, and a reused slot's new pipeline from the pool", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true, .framesInFlight = 2 });
    const ShaderLibrary library = standard(device);
    const Pipeline a = vertexPipeline(device, library);
    const Pipeline b = vertexPipeline(device, library, RasterState{ .cull = CullMode::Back });
    const Pipeline c = vertexPipeline(device, library, RasterState{ .cull = CullMode::Front });
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    using Pair = std::array<uint64_t, 2>;
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    CHECK(drawOf(list).resolved.handles == Pair{});
    list.beginRenderPass(passOf(colors));
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    list.setPipeline(a);
    list.draw(3);
    list.setPipeline(b);
    list.draw(3);
    // The two handles bound last, newest first, with their payloads.
    const metal4::ResolvedPipelines& resolved = drawOf(list).resolved;
    CHECK(resolved.handles == Pair{ b.bits, a.bits });
    CHECK(resolved.states == std::array{ stateOf(list, b), stateOf(list, a) });
    // Binding either again reads the pair and leaves it as it is.
    list.setPipeline(a);
    list.draw(3);
    CHECK(drawOf(list).held.pipeline == stateOf(list, a));
    CHECK(resolved.handles == Pair{ b.bits, a.bits });
    // A third pipeline replaces the older entry.
    list.setPipeline(c);
    list.draw(3);
    CHECK(drawOf(list).held.pipeline == stateOf(list, c));
    CHECK(resolved.handles == Pair{ c.bits, b.bits });
    list.endRenderPass();
    // The pair outlives the pass: the next pass binds b from it.
    list.beginRenderPass(passOf(colors));
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    list.setPipeline(b);
    list.draw(3);
    CHECK(drawOf(list).held.pipeline == stateOf(list, b));
    CHECK(resolved.handles == Pair{ c.bits, b.bits });
    list.endRenderPass();
    // The list ends its frame holding a full pair; its frame slot hands the same list out again below.
    const CommandListImpl* recycled = &CommandListImpl::of(list);
    finish(device, list);

    // Once a retires, a new pipeline takes its slot at the next generation.
    device.destroy(a);
    for (int frame = 0; frame < 3; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    const Pipeline d = vertexPipeline(device, library, RasterState{ .cull = CullMode::Back });
    using Pool = core::HandlePool<Pipeline, core::PipelineRecord>;
    REQUIRE(Pool::slotIndex(d) == Pool::slotIndex(a));
    REQUIRE(d.bits != a.bits);
    device.beginFrame();
    list = device.acquireCommandList(QueueType::Graphics, "test.list");
    // Acquisition empties the pair the recycled list held. Even an entry left for a's handle names another generation of the
    // slot, so d misses it and is read from the pool.
    REQUIRE(&CommandListImpl::of(list) == recycled);
    CHECK(drawOf(list).resolved.handles == Pair{});
    CHECK(drawOf(list).resolved.states == std::array<const metal4::PipelineState*, 2>{});
    metal4::ResolvedPipelines& planted = CommandListImpl::of(list).draw.resolved;
    planted.handles = { a.bits, 0 };
    planted.states = { stateOf(list, b), nullptr };
    list.beginRenderPass(passOf(colors));
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    list.setPipeline(d);
    list.draw(3);
    CHECK(drawOf(list).held.pipeline == stateOf(list, d));
    CHECK(drawOf(list).resolved.handles == Pair{ d.bits, a.bits });
    list.endRenderPass();
    finish(device, list);
    destroyDevice(device);
}

TEST_CASE("dynamic state equal to the encoder's sends no call; a new encoder after a split replays the held state", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const ShaderLibrary library = standard(device);
    // Every raster field differs from Metal's encoder default.
    const Pipeline pipeline = vertexPipeline(
        device,
        library,
        RasterState{ .cull = CullMode::Back, .frontFace = FrontFace::CounterClockwise, .fill = FillMode::Wireframe, .depthClip = false, .depthBias = true }
    );
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(colors));
    list.setPipeline(pipeline);
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    list.draw(3);
    uint64_t before = callsOf(list);
    // The values the encoder holds, and the starting values of the stencil reference and the blend color.
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    list.setStencilReference(0);
    list.setBlendConstants(0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(callsOf(list) == before);
    list.setStencilReference(7);
    list.setBlendConstants(0.5f, 0.25f, 1.0f, 1.0f);
    list.setViewport(Viewport{ .x = 1.0f, .width = 8.0f, .height = 8.0f });
    list.setScissor(Rect2D{ 1, 1, 8, 8 });
    list.setDepthBias(1.0f, 2.0f, 3.0f);
    CHECK(callsOf(list) - before == 5);
    // The second set of equal values sends nothing.
    before = callsOf(list);
    list.setStencilReference(7);
    list.setDepthBias(1.0f, 2.0f, 3.0f);
    CHECK(callsOf(list) == before);
    const metal4::RenderHeld held = drawOf(list).held;
    // The split stores the attachment, created with Store, at no cost and ends the encoder.
    metal4::splitPass(CommandListImpl::of(list));
    before = callsOf(list);
    // The open, then the held state of the encoder the split ended: the pipeline, the cull mode, the winding, the fill mode,
    // the depth clip mode, the bias, the viewport, the scissor, the stencil reference and the blend color; then the table
    // and the draw.
    list.draw(3);
    CHECK(callsOf(list) - before == openCalls(list) + 10 + kTable + 1);
    CHECK(countsOf(list).encodersOpened == 2);
    const metal4::RenderHeld& replayed = drawOf(list).held;
    CHECK(replayed.pipeline == held.pipeline);
    CHECK(replayed.cull == MTL::CullModeBack);
    CHECK(replayed.winding == MTL::WindingCounterClockwise);
    CHECK(replayed.fill == MTL::TriangleFillModeLines);
    CHECK(replayed.depthClip == MTL::DepthClipModeClamp);
    CHECK(replayed.bias == metal4::DepthBias{ 1.0f, 2.0f, 3.0f });
    CHECK(replayed.stencilReference == 7);
    CHECK(replayed.blendColor == held.blendColor);
    CHECK(replayed.viewport.originX == 1.0);
    CHECK(replayed.scissor.x == 1);
    list.endRenderPass();
    finish(device, list);
    destroyDevice(device);
}

TEST_CASE("a pipeline without depthBias applies zero bias and the next depthBias pipeline restores the recorded values", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const ShaderLibrary library = standard(device);
    const Pipeline biased = vertexPipeline(device, library, RasterState{ .depthBias = true });
    const Pipeline plain = vertexPipeline(device, library);
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(colors));
    uint64_t before = callsOf(list);
    list.setPipeline(biased);
    list.setDepthBias(1.0f, 2.0f, 3.0f);
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    // The open and the pass's begin timestamp, the pipeline and its winding (its bias, still zero, equals the encoder's), the
    // bias, the viewport, the scissor, the table and the draw.
    list.draw(3);
    CHECK(callsOf(list) - before == openCalls(list) + kPassBegin + 5 + kTable + 1);
    const metal4::RenderHeld& held = drawOf(list).held;
    CHECK(held.bias == metal4::DepthBias{ 1.0f, 2.0f, 3.0f });
    // The pipeline and a zero bias.
    before = callsOf(list);
    list.setPipeline(plain);
    CHECK(callsOf(list) - before == 2);
    CHECK(held.bias == metal4::DepthBias{});
    CHECK(drawOf(list).bias == metal4::DepthBias{ 1.0f, 2.0f, 3.0f });
    // The pipeline and the recorded values again.
    before = callsOf(list);
    list.setPipeline(biased);
    CHECK(callsOf(list) - before == 2);
    CHECK(held.bias == metal4::DepthBias{ 1.0f, 2.0f, 3.0f });
    list.endRenderPass();
    // The values live for one pass: the next starts at zero, which a fresh encoder holds, so its depthBias pipeline sets none.
    list.beginRenderPass(passOf(colors));
    list.setPipeline(biased);
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    before = callsOf(list);
    // The open and the pass's begin timestamp, the pipeline and its winding, the viewport, the scissor, the table and the
    // draw.
    list.draw(3);
    CHECK(callsOf(list) - before == openCalls(list) + kPassBegin + 4 + kTable + 1);
    CHECK(drawOf(list).bias == metal4::DepthBias{});
    list.endRenderPass();
    finish(device, list);
    destroyDevice(device);
}

TEST_CASE("a depth bias recorded before a pipeline in the prefix reaches the encoder through the pipeline's replay", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const ShaderLibrary library = standard(device);
    const Pipeline first = vertexPipeline(device, library, RasterState{ .depthBias = true });
    const Pipeline second = vertexPipeline(device, library, RasterState{ .depthBias = true });
    const Pipeline plain = vertexPipeline(device, library);
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(colors));
    // The later pipeline takes the earlier one's place, so the prefix holds the pipeline, then the bias.
    list.setPipeline(first);
    list.setDepthBias(1.0f, 2.0f, 3.0f);
    list.setPipeline(second);
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    list.draw(3);
    const metal4::RenderHeld& held = drawOf(list).held;
    CHECK(held.pipeline == stateOf(list, second));
    CHECK(held.bias == metal4::DepthBias{ 1.0f, 2.0f, 3.0f });
    // After a split, the pipeline bound before it is the one setDepthBias needs, so the bias comes before the pipeline in
    // the prefix. The pipeline replayed after it applies the bias it saw: here the plain pipeline's zero.
    metal4::splitPass(CommandListImpl::of(list));
    list.setDepthBias(4.0f, 5.0f, 6.0f);
    list.setPipeline(plain);
    using Kind = metal4::PrefixKind;
    CHECK(prefixKinds(list) == std::vector{ Kind::DepthBias, Kind::Pipeline });
    list.draw(3);
    CHECK(held.pipeline == stateOf(list, plain));
    CHECK(held.bias == metal4::DepthBias{});
    CHECK(drawOf(list).bias == metal4::DepthBias{ 4.0f, 5.0f, 6.0f });
    // Splitting again, with the plain pipeline held: the bias entry replayed after a depthBias pipeline applies its values.
    metal4::splitPass(CommandListImpl::of(list));
    list.setPipeline(second);
    list.setDepthBias(7.0f, 8.0f, 9.0f);
    list.setPipeline(first);
    list.draw(3);
    CHECK(held.pipeline == stateOf(list, first));
    CHECK(held.bias == metal4::DepthBias{ 7.0f, 8.0f, 9.0f });
    list.endRenderPass();
    finish(device, list);
    destroyDevice(device);
}

TEST_CASE("a stencil-only pipeline with no fragment entry sets the winding and a depth-only one does not", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const ShaderLibrary library = standard(device);
    const StencilFaceState replace{ .pass = StencilOp::Replace };
    const Pipeline stencil = *device.createPipeline(
        GraphicsPipelineDesc{ .vertex = { library, "vsMain" },
                              .depthStencil = { .stencilTest = true, .front = replace, .back = replace },
                              .depthFormat = Format::D32FloatS8,
                              .label = "test.stencil" }
    );
    const Pipeline depth = *device.createPipeline(
        GraphicsPipelineDesc{
            .vertex = { library, "vsMain" }, .depthStencil = { .depthTest = true, .depthWrite = true }, .depthFormat = Format::D32Float, .label = "test.depth" }
    );
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    CHECK(stateOf(list, stencil)->setsWinding);
    CHECK_FALSE(stateOf(list, depth)->setsWinding);
    const Texture stencilTarget = makeTexture(device, Format::D32FloatS8, TextureUsage::DepthStencil);
    const Texture depthTarget = makeTexture(device, Format::D32Float, TextureUsage::DepthStencil);
    list.beginRenderPass(passOf({}, DepthAttachment{ .texture = stencilTarget, .load = LoadOp::Clear, .stencilLoad = LoadOp::Clear }));
    list.setPipeline(stencil);
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    list.draw(3);
    CHECK(drawOf(list).held.winding == MTL::WindingCounterClockwise);
    list.endRenderPass();
    list.beginRenderPass(passOf({}, DepthAttachment{ .texture = depthTarget, .load = LoadOp::Clear }));
    list.setPipeline(depth);
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    list.draw(3);
    CHECK(drawOf(list).held.winding == MTL::WindingClockwise);
    list.endRenderPass();
    finish(device, list);
    destroyDevice(device);
}

TEST_CASE("a point is drawn front-facing, on the front stencil state, under every cull mode and winding", "[metal4]")
{
    // A Point pipeline sends no winding, which rests on Metal facing a point the same under either winding; Apple documents
    // the face only by vertex order. Each case primes the encoder's winding with a pipeline that sets it, then draws a point
    // whose front stencil operation replaces with the reference and whose back one increments, so the stencil tells the
    // face Metal applied, and a probe that passes only where the front operation ran.
    constexpr uint32_t kCell = 16;
    constexpr uint8_t kReference = 2;
    struct Case
    {
        CullMode cull;
        FrontFace face;
    };
    std::vector<Case> cases;
    for (const CullMode cull : { CullMode::None, CullMode::Front, CullMode::Back })
    {
        for (const FrontFace face : { FrontFace::CounterClockwise, FrontFace::Clockwise })
        {
            cases.push_back(Case{ cull, face });
        }
    }
    const auto width = static_cast<uint32_t>(cases.size()) * kCell;
    test::GpuKit kit;
    const std::vector<std::byte> blob = test::shaderLibraryFile("PointFaceOracle");
    const ShaderLibrary library = *kit.device.loadShaderLibrary(blob, "test.pointFace");
    const auto pipelineOf = [&](GraphicsPipelineDesc desc, ColorWriteMask face, ColorWriteMask probe) {
        const std::array colors{ ColorTarget{ .format = Format::RGBA8Unorm, .writeMask = face },
                                 ColorTarget{ .format = Format::RGBA8Unorm, .writeMask = probe } };
        desc.colors = colors;
        desc.depthFormat = Format::D32FloatS8;
        desc.label = "test.pipeline";
        const Result<Pipeline> pipeline = kit.device.createPipeline(desc);
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    };
    // The probe culls nothing and has a fragment entry, so binding it sets its winding on the encoder.
    const StencilFaceState equal{ .compare = CompareOp::Equal };
    const auto probeOf = [&](FrontFace face) {
        return pipelineOf(
            GraphicsPipelineDesc{ .vertex = { library, "probeVertexMain" },
                                  .fragment = { library, "probeFragmentMain" },
                                  .raster = { .frontFace = face },
                                  .depthStencil = { .stencilTest = true, .front = equal, .back = equal } },
            ColorWriteMask::None,
            ColorWriteMask::All
        );
    };
    const std::array probes{ probeOf(FrontFace::CounterClockwise), probeOf(FrontFace::Clockwise) };
    const StencilFaceState replace{ .pass = StencilOp::Replace };
    const StencilFaceState increment{ .pass = StencilOp::IncrementClamp };
    const auto textureOf = [&](Format format, TextureUsage usage) {
        return *kit.device.createTexture(
            TextureDesc{ .type = TextureType::Tex2D, .format = format, .extent = { width, kCell, 1 }, .usage = usage, .label = "test.target" }
        );
    };
    const Texture face = textureOf(Format::RGBA8Unorm, TextureUsage::RenderTarget);
    const Texture probe = textureOf(Format::RGBA8Unorm, TextureUsage::RenderTarget);
    const Texture depth = textureOf(Format::D32FloatS8, TextureUsage::DepthStencil);
    CommandList list = kit.list(QueueType::Graphics, "test.pointFace");
    test::transition(list, face, Layout::Undefined, Layout::ColorAttachment);
    test::transition(list, probe, Layout::Undefined, Layout::ColorAttachment);
    test::transition(list, depth, Layout::Undefined, Layout::DepthStencilWrite);
    const std::array colors{ ColorAttachment{ .texture = face, .load = LoadOp::Clear }, ColorAttachment{ .texture = probe, .load = LoadOp::Clear } };
    list.beginRenderPass(
        RenderPassDesc{
            .colors = colors,
            .depth =
                DepthAttachment{
                    .texture = depth, .load = LoadOp::Clear, .store = StoreOp::DontCare, .stencilLoad = LoadOp::Clear, .stencilStore = StoreOp::DontCare },
            .area = { 0, 0, width, kCell },
            .label = "test.pass" }
    );
    list.setStencilReference(kReference);
    for (uint32_t i = 0; i < cases.size(); ++i)
    {
        CAPTURE(i);
        const Pipeline point = pipelineOf(
            GraphicsPipelineDesc{ .vertex = { library, "pointFaceVertexMain" },
                                  .fragment = { library, "pointFaceFragmentMain" },
                                  .primitive = PrimitiveClass::Point,
                                  .raster = { .cull = cases[i].cull, .frontFace = cases[i].face },
                                  .depthStencil = { .stencilTest = true, .front = replace, .back = increment } },
            ColorWriteMask::All,
            ColorWriteMask::None
        );
        const Pipeline primer = probes[cases[i].face == FrontFace::Clockwise ? 1 : 0];
        list.setViewport(Viewport{ .x = static_cast<float>(i * kCell), .width = kCell, .height = kCell });
        list.setScissor(Rect2D{ i * kCell, 0, kCell, kCell });
        // The probe sets the case's winding and writes nothing: no texel of the cell holds the reference yet.
        list.setPipeline(primer);
        list.draw(3);
        list.setPipeline(point);
        list.draw(1);
        // The encoder holds the case's winding as the point draws.
        CHECK(drawOf(list).held.winding == (cases[i].face == FrontFace::Clockwise ? MTL::WindingClockwise : MTL::WindingCounterClockwise));
        list.setPipeline(primer);
        list.draw(3);
    }
    list.endRenderPass();
    test::transition(list, face, Layout::ColorAttachment, Layout::CopySrc);
    test::transition(list, probe, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    const TextureRegion region{ .extent = { width, kCell, 1 } };
    const std::vector<std::byte> faces = kit.readTexture(face, region, Format::RGBA8Unorm);
    const std::vector<std::byte> probed = kit.readTexture(probe, region, Format::RGBA8Unorm);
    REQUIRE(faces.size() == size_t{ width } * kCell * 4);
    REQUIRE(probed.size() == faces.size());
    const auto texel = [&](const std::vector<std::byte>& bytes, uint32_t x, uint32_t y) {
        const size_t at = (size_t{ y } * width + x) * 4;
        return std::format("{} {} {} {}", int(bytes[at]), int(bytes[at + 1]), int(bytes[at + 2]), int(bytes[at + 3]));
    };
    // Each cell holds one point at its pixel (8, 8): drawn, red for a front face in the face target, and blue in the probe
    // target, where the stencil holds the reference the front operation wrote. Every other texel stays clear.
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < kCell; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const bool point = x % kCell == 8 && y == 8;
            const std::string expectedFace = point ? "255 0 0 255" : "0 0 0 0";
            const std::string expectedProbe = point ? "0 0 255 255" : "0 0 0 0";
            if (texel(faces, x, y) != expectedFace || texel(probed, x, y) != expectedProbe)
            {
                if (++wrong <= 8)
                {
                    UNSCOPED_INFO(
                        std::format(
                            "case {} ({}, {}): face {}, expected {}; probe {}, expected {}",
                            x / kCell,
                            x % kCell,
                            y,
                            texel(faces, x, y),
                            expectedFace,
                            texel(probed, x, y),
                            expectedProbe
                        )
                    );
                }
            }
        }
    }
    CHECK(wrong == 0);
}

TEST_CASE("setRoot writes nothing until a draw or a dispatch reads the root, and an unchanged root sends nothing", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true, .debugMarkers = true });
    const ShaderLibrary library = standard(device);
    const Pipeline graphics = vertexPipeline(device, library);
    const Pipeline compute = *device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" }, .label = "test.compute" });
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const Addr first = uploadRoot(list);
    const Addr second = uploadRoot(list);
    const Addr third = uploadRoot(list);
    // Outside a pass the root waits too: the dispatch writes it, once, and an unchanged root sends nothing.
    list.setPipeline(compute);
    uint64_t before = callsOf(list);
    list.setRoot(first);
    list.setRoot(second);
    CHECK(callsOf(list) == before);
    CHECK(drawOf(list).tableRoot == 0);
    list.dispatch(1, 1, 1);
    CHECK(callsOf(list) - before == 1 + 1);
    CHECK(drawOf(list).tableRoot == gpuAddressOf(second));
    list.setRoot(second);
    before = callsOf(list);
    list.dispatch(1, 1, 1);
    CHECK(callsOf(list) - before == 1);
    list.setRoot(first);
    before = callsOf(list);
    list.dispatch(1, 1, 1);
    CHECK(callsOf(list) - before == 1 + 1);
    CHECK(drawOf(list).tableRoot == gpuAddressOf(first));
    // Inside a pass, before its encoder opens, the root is no prefix entry either; the first draw writes the last one.
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(colors));
    list.setPipeline(graphics);
    before = callsOf(list);
    list.setRoot(second);
    list.setRoot(third);
    CHECK(callsOf(list) == before);
    CHECK(drawOf(list).tableRoot == gpuAddressOf(first));
    // The encoder draws nothing, so it binds no table and writes no root, and stays open.
    list.endRenderPass();
    CHECK(drawOf(list).tableRoot == gpuAddressOf(first));
    list.beginRenderPass(passOf(colors));
    list.setPipeline(graphics);
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    list.draw(3);
    CHECK(drawOf(list).tableRoot == gpuAddressOf(third));
    // An open encoder writes nothing at `setRoot` either: the next draw does.
    before = callsOf(list);
    list.setRoot(second);
    CHECK(callsOf(list) == before);
    list.draw(3);
    CHECK(callsOf(list) - before == 1 + 1);
    CHECK(drawOf(list).tableRoot == gpuAddressOf(second));
    list.draw(3);
    CHECK(callsOf(list) - before == 1 + 1 + 1);
    list.endRenderPass();
    // A pass that only executes or draws leaves its root in the table, and a dispatch after it writes the one recorded since.
    list.setRoot(first);
    list.setPipeline(compute);
    before = callsOf(list);
    list.dispatch(1, 1, 1);
    CHECK(callsOf(list) - before == 1 + 1);
    CHECK(drawOf(list).tableRoot == gpuAddressOf(first));
    finish(device, list);
    destroyDevice(device);
}

TEST_CASE("a compute pipeline bound before an unequal fill, or before a scope boundary, is set again on the dispatch path, once", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const ShaderLibrary library = standard(device);
    const Pipeline pipeline = *device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" }, .label = "test.compute" });
    const Buffer buffer = *device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.buffer" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    CommandListImpl& impl = CommandListImpl::of(list);
    const MTL::ComputePipelineState* compute = stateOf(list, pipeline)->compute;
    // Binding opens the lazy compute encoder (the factory, the label and the table) and sets the pipeline.
    uint64_t before = callsOf(list);
    list.setPipeline(pipeline);
    CHECK(callsOf(list) - before == openCalls(list) + kTable + 1);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(encodersOf(list).kernelPipeline() == compute);
    CHECK(drawOf(list).compute == stateOf(list, pipeline));
    SECTION("a fill whose bytes differ runs the fill kernel")
    {
        list.fillBuffer(buffer, 0, 256, 0x01020304u);
        CHECK(encodersOf(list).kernelPipeline() != compute);
        before = callsOf(list);
        metal4::applyComputePipeline(impl);
        CHECK(callsOf(list) - before == 1);
        CHECK(encodersOf(list).kernelPipeline() == compute);
        // Once: the encoder now holds the caller's pipeline.
        before = callsOf(list);
        metal4::applyComputePipeline(impl);
        CHECK(callsOf(list) == before);
    }
    SECTION("a scope boundary closes the encoder")
    {
        list.beginScope("test.scope");
        CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
        before = callsOf(list);
        // The dispatch path opens the encoder again and sets the pipeline; the list uses no carry fence, since no counted
        // draw has run on the device.
        metal4::applyComputePipeline(impl);
        CHECK(callsOf(list) - before == openCalls(list) + kTable + 1);
        CHECK(encodersOf(list).kernelPipeline() == compute);
        before = callsOf(list);
        metal4::applyComputePipeline(impl);
        CHECK(callsOf(list) == before);
        list.endScope();
    }
    finish(device, list);
    destroyDevice(device);
}

TEST_CASE("setPipeline with a compute pipeline, then a barrier, emits the barrier on the open encoder before the next dispatch", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const ShaderLibrary library = standard(device);
    const Pipeline pipeline = *device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" }, .label = "test.compute" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.setPipeline(pipeline);
    REQUIRE(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    REQUIRE_FALSE(encodersOf(list).holdsWork());
    // The encoder holds no work, so the barrier's sources lie in earlier encoders: it waits as the pending barrier.
    list.barrier(
        std::array{ GlobalBarrier{
            .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderRead } }
    );
    const metal4::BarrierState& barriers = encodersOf(list).barriers();
    CHECK(countsOf(list).nativeBarriers == 0);
    CHECK(barriers.pendingDestination == MTL::StageDispatch);
    // The dispatch path emits it on the open encoder, which keeps its pipeline: the barrier alone, since the list uses no
    // carry fence (no counted draw has run on the device).
    const uint64_t before = callsOf(list);
    metal4::applyComputePipeline(CommandListImpl::of(list));
    CHECK(callsOf(list) - before == 1);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(barriers.pendingDestination == 0);
    CHECK((barriers.openDestination & MTL::StageDispatch) != 0);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    finish(device, list);
    destroyDevice(device);
}

TEST_CASE("direct draws read drawId 0 from the device's zero record, which a list's table holds from its creation", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const ShaderLibrary library = standard(device);
    const std::array targets{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Pipeline mesh = *device.createPipeline(
        GraphicsPipelineDesc{ .mesh = { library, "msMain" }, .fragment = { library, "fsMain" }, .colors = targets, .label = "test.mesh" }
    );
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    const MTL::Buffer* zero = DeviceImpl::of(device).zeroRecord;
    REQUIRE(zero != nullptr);
    REQUIRE(zero->length() >= 4);
    // The record holds zeroes.
    const auto* bytes = static_cast<const std::byte*>(const_cast<MTL::Buffer*>(zero)->contents());
    CHECK(bytes[0] == std::byte{ 0 });
    CHECK(bytes[1] == std::byte{ 0 });
    CHECK(bytes[2] == std::byte{ 0 });
    CHECK(bytes[3] == std::byte{ 0 });
    device.beginFrame();
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    for (int recording = 0; recording < 2; ++recording)
    {
        CAPTURE(recording);
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        CHECK(drawOf(list).zeroRecord == zero->gpuAddress());
        CHECK(drawOf(list).tableDrawRecord == zero->gpuAddress());
        list.beginRenderPass(passOf(colors));
        list.setPipeline(mesh);
        list.setViewport(kViewport);
        list.setScissor(kScissor);
        uint64_t before = callsOf(list);
        // The open and the pass's begin timestamp, the table at the object and mesh stages, the pipeline and its winding, the
        // viewport and the scissor, then the table at the vertex and fragment stages and the draw: the table holds the zero
        // record already.
        list.drawMeshTasks(1, 1, 1);
        CHECK(callsOf(list) - before == openCalls(list) + kPassBegin + 5 + kTable + 1);
        before = callsOf(list);
        list.drawMeshTasks(2, 1, 1);
        CHECK(callsOf(list) - before == 1);
        CHECK(drawOf(list).tableDrawRecord == zero->gpuAddress());
        list.endRenderPass();
        list.end();
        device.queue(QueueType::Graphics).submit(std::array{ list });
    }
    device.endFrame();
    destroyDevice(device);
}

TEST_CASE("a render encoder binds the list's table to the object and mesh stages at its first mesh pipeline only", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const ShaderLibrary library = standard(device);
    const std::array targets{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Pipeline plain = vertexPipeline(device, library);
    const Pipeline mesh = *device.createPipeline(
        GraphicsPipelineDesc{ .mesh = { library, "msMain" }, .fragment = { library, "fsMain" }, .colors = targets, .label = "test.mesh" }
    );
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    for (int pass = 0; pass < 2; ++pass)
    {
        CAPTURE(pass);
        list.beginRenderPass(passOf(colors));
        list.setViewport(kViewport);
        list.setScissor(kScissor);
        list.setPipeline(plain);
        list.draw(3);
        // The encoder opened with the table at the vertex and fragment stages alone.
        CHECK_FALSE(drawOf(list).held.meshStages);
        uint64_t before = callsOf(list);
        // The table at the object and mesh stages, then the pipeline; the two share their winding.
        list.setPipeline(mesh);
        CHECK(callsOf(list) - before == 2);
        CHECK(drawOf(list).held.meshStages);
        list.drawMeshTasks(1, 1, 1);
        before = callsOf(list);
        // A mesh pipeline set again on the same encoder sends only the pipelines.
        list.setPipeline(plain);
        list.setPipeline(mesh);
        CHECK(callsOf(list) - before == 2);
        list.endRenderPass();
    }
    list.end();
    device.queue(QueueType::Graphics).submit(std::array{ list });
    device.endFrame();
    destroyDevice(device);
}

TEST_CASE("a zero-count draw opens no encoder", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const ShaderLibrary library = standard(device);
    const Pipeline pipeline = vertexPipeline(device, library);
    const std::array targets{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Pipeline mesh = *device.createPipeline(
        GraphicsPipelineDesc{ .mesh = { library, "msMain" }, .fragment = { library, "fsMain" }, .colors = targets, .label = "test.mesh" }
    );
    const Buffer indices = *device.createBuffer({ .size = 256, .usage = BufferUsage::Index, .label = "test.indices" });
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(colors));
    list.setPipeline(pipeline);
    list.setViewport(kViewport);
    list.setScissor(kScissor);
    const uint64_t before = callsOf(list);
    list.draw(0);
    list.draw(3, 0);
    list.drawIndexed(indices, IndexType::UInt16, 0);
    list.drawIndexed(indices, IndexType::UInt32, 3, 0);
    list.setPipeline(mesh);
    list.drawMeshTasks(0, 1, 1);
    list.drawMeshTasks(1, 1, 0);
    CHECK(callsOf(list) == before);
    CHECK(countsOf(list).encodersOpened == 0);
    CHECK(encodersOf(list).renderEncoder() == nullptr);
    list.endRenderPass();
    finish(device, list);
    destroyDevice(device);
}

TEST_CASE("C8's sequence makes the calls the overhead note registers", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true, .debugMarkers = true });
    const ShaderLibrary library = standard(device);
    const Pipeline pipeline = vertexPipeline(device, library);
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    const std::array between{ TextureBarrier{ .texture = target,
                                              .srcStage = Stage::ColorAttachment,
                                              .srcAccess = Access::ColorAttachmentWrite,
                                              .dstStage = Stage::ColorAttachment,
                                              .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                                              .oldLayout = Layout::ColorAttachment,
                                              .newLayout = Layout::ColorAttachment } };
    for (int repetition = 0; repetition < 3; ++repetition)
    {
        CAPTURE(repetition);
        const uint64_t before = callsOf(list);
        const uint64_t barriers = countsOf(list).nativeBarriers;
        if (repetition > 0)
        {
            list.barrier({}, {}, between);
        }
        list.beginRenderPass(passOf(colors));
        list.setPipeline(pipeline);
        list.setViewport(kViewport);
        list.setScissor(kScissor);
        list.draw(3);
        list.endRenderPass();
        // After the first pass, endEncoding of the previous pass's encoder, which stayed open; pushDebugGroup,
        // writeTimestampIntoHeap; renderCommandEncoder, setLabel; the consumer barrier after the first pass;
        // setRenderPipelineState, setFrontFacingWinding, setViewport, setScissorRect, setArgumentTable, drawPrimitives;
        // writeTimestamp on the encoder and popDebugGroup. The winding call, one per encoder, is beyond the registered sequence: the pipeline has a
        // fragment entry, which may read the front face, and its counter-clockwise winding is not Metal's default.
        const uint64_t barrier = repetition > 0 ? 1 : 0;
        CHECK(callsOf(list) - before == barrier + 2 + openCalls(list) + barrier + 5 + kTable + 2);
        CHECK(countsOf(list).nativeBarriers - barriers == barrier);
    }
    finish(device, list);
    destroyDevice(device);
}
