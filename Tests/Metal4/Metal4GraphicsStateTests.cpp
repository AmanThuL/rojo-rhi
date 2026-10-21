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
#include "Support/Metal4/Metal4Timing.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The viewport and scissor that cover the 16-texel square targets of these cases.
    constexpr Viewport kViewport{ .width = 16.0f, .height = 16.0f };
    constexpr Rect2D kScissor{ 0, 0, 16, 16 };

    //==================================================================================================================
    /// The calls a pass's render encoder makes when it opens with no barrier owed: the factory, the label when `list`'s
    /// encoders carry one, and the table.
    uint64_t openCalls(CommandList list)
    {
        return 2 + test::encoderLabel(list);
    }

    /// The call of the begin timestamp a pass's first render encoder writes right after it opens.
    constexpr uint64_t kPassBegin = 1;

    //==================================================================================================================
    /// The encoders of `list`.
    const metal4::Encoders& encodersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders;
    }

    //==================================================================================================================
    /// The graphics state of `list`.
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
    /// The native calls `list` has recorded so far.
    uint64_t callsOf(CommandList list)
    {
        return countsOf(list).nativeRecordCalls;
    }

    //==================================================================================================================
    /// The payload of `pipeline` on the device of `list`.
    const metal4::PipelineState* stateOf(CommandList list, Pipeline pipeline)
    {
        return static_cast<const metal4::PipelineState*>(core::coreOf(list).device->pool<Pipeline>().native(pipeline));
    }

    //==================================================================================================================
    /// The GPU address `address` names.
    uint64_t gpuAddressOf(Addr address)
    {
        return uint64_t{ address.lo } | uint64_t{ address.hi } << 32;
    }

    //==================================================================================================================
    /// Loads the standard library on `device`.
    ShaderLibrary standard(Device device)
    {
        const std::vector<std::byte> blob = test::standardLibrary();
        return *device.loadShaderLibrary(blob, "test.library");
    }

    //==================================================================================================================
    /// Creates a vertex pipeline of the standard library over one `RGBA8Unorm` target and `depth`, with `raster` and
    /// `depthStencil`.
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

    //==================================================================================================================
    /// Creates a 16-texel square 2D texture of `format` with `usage`.
    Texture makeTexture(Device device, Format format, TextureUsage usage = TextureUsage::RenderTarget)
    {
        return *device.createTexture(
            TextureDesc{ .type = TextureType::Tex2D, .format = format, .extent = { 16, 16, 1 }, .usage = usage, .label = "test.target" }
        );
    }

    //==================================================================================================================
    /// A pass over `colors` and `depth` that renders to the whole 16-texel square.
    RenderPassDesc passOf(std::span<const ColorAttachment> colors, const DepthAttachment& depth = {})
    {
        return RenderPassDesc{ .colors = colors, .depth = depth, .area = { 0, 0, 16, 16 }, .label = "test.pass" };
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
    /// Uploads a 256-byte root of zeroes into the frame and returns its address.
    Addr uploadRoot(CommandList list)
    {
        const std::array<std::byte, 256> zeroes{};
        return list.uploadFrameData(zeroes, 256);
    }

    //==================================================================================================================
    /// The kinds of `list`'s prefix, in order.
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

//======================================================================================================================
TEST_CASE("state recorded before the first draw waits in the prefix and replays in order at the open", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
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
    CHECK(prefixKinds(list) == std::vector{ Kind::Pipeline, Kind::Viewport, Kind::Scissor, Kind::Root, Kind::StencilReference, Kind::BlendColor });
    // The open and the pass's begin timestamp, then each entry in order: the pipeline with its counter-clockwise winding (it
    // has a fragment entry; the rest of its raster state is Metal's default), the viewport, the scissor, the table's root,
    // the stencil reference and the blend color; then the draw.
    list.draw(3);
    CHECK(callsOf(list) == base + openCalls(list) + kPassBegin + 7 + 1);
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
    // and its group; the encoder stays open until the list ends.
    list.endRenderPass();
    CHECK(callsOf(list) == before + openCalls(list) + kPassBegin + 3 + 2);
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
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

//======================================================================================================================
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

//======================================================================================================================
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
    // the depth clip mode, the bias, the viewport, the scissor, the stencil reference and the blend color; then the draw.
    list.draw(3);
    CHECK(callsOf(list) - before == openCalls(list) + 10 + 1);
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

//======================================================================================================================
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
    // bias, the viewport, the scissor and the draw.
    list.draw(3);
    CHECK(callsOf(list) - before == openCalls(list) + kPassBegin + 5 + 1);
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
    // The open and the pass's begin timestamp, the pipeline and its winding, the viewport, the scissor and the draw.
    list.draw(3);
    CHECK(callsOf(list) - before == openCalls(list) + kPassBegin + 4 + 1);
    CHECK(drawOf(list).bias == metal4::DepthBias{});
    list.endRenderPass();
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
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

//======================================================================================================================
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

//======================================================================================================================
TEST_CASE("setRoot outside a pass writes the table at once and an unchanged root sends nothing", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const Addr first = uploadRoot(list);
    const Addr second = uploadRoot(list);
    uint64_t before = callsOf(list);
    list.setRoot(first);
    CHECK(callsOf(list) - before == 1);
    CHECK(drawOf(list).tableRoot == gpuAddressOf(first));
    list.setRoot(first);
    CHECK(callsOf(list) - before == 1);
    list.setRoot(second);
    CHECK(callsOf(list) - before == 2);
    CHECK(drawOf(list).tableRoot == gpuAddressOf(second));
    // Inside a pass, before its encoder opens, the root waits in the prefix; the open writes the last one.
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(colors));
    before = callsOf(list);
    list.setRoot(second);
    list.setRoot(first);
    CHECK(callsOf(list) == before);
    CHECK(drawOf(list).tableRoot == gpuAddressOf(second));
    // The open and the pass's begin timestamp, the root, the pass's end timestamp and its group; the encoder stays open.
    list.endRenderPass();
    CHECK(callsOf(list) - before == openCalls(list) + kPassBegin + 1 + 2);
    CHECK(drawOf(list).tableRoot == gpuAddressOf(first));
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
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
    CHECK(callsOf(list) - before == openCalls(list) + 1);
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
        CHECK(callsOf(list) - before == openCalls(list) + 1);
        CHECK(encodersOf(list).kernelPipeline() == compute);
        before = callsOf(list);
        metal4::applyComputePipeline(impl);
        CHECK(callsOf(list) == before);
        list.endScope();
    }
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
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

//======================================================================================================================
TEST_CASE("direct mesh draws bind one zero record per recording", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const ShaderLibrary library = standard(device);
    const std::array targets{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Pipeline mesh = *device.createPipeline(
        GraphicsPipelineDesc{ .mesh = { library, "msMain" }, .fragment = { library, "fsMain" }, .colors = targets, .label = "test.mesh" }
    );
    const Texture target = makeTexture(device, Format::RGBA8Unorm);
    device.beginFrame();
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    for (int recording = 0; recording < 2; ++recording)
    {
        CAPTURE(recording);
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        CHECK(drawOf(list).zeroRecord == 0);
        list.beginRenderPass(passOf(colors));
        list.setPipeline(mesh);
        list.setViewport(kViewport);
        list.setScissor(kScissor);
        uint64_t before = callsOf(list);
        // The open and the pass's begin timestamp, the table at the object and mesh stages, the pipeline and its winding, the
        // viewport and the scissor, the zero record at buffer(3), then the draw.
        list.drawMeshTasks(1, 1, 1);
        CHECK(callsOf(list) - before == openCalls(list) + kPassBegin + 5 + 1 + 1);
        const uint64_t record = drawOf(list).zeroRecord;
        REQUIRE(record != 0);
        CHECK(record % 16 == 0);
        CHECK(drawOf(list).tableDrawRecord == record);
        // The record lies in the list's arena page and holds zeroes.
        const core::ArenaPage& page = core::coreOf(list).page;
        REQUIRE(record >= page.address);
        const std::byte* bytes = page.host + (record - page.address);
        CHECK(bytes[0] == std::byte{ 0 });
        CHECK(bytes[1] == std::byte{ 0 });
        CHECK(bytes[2] == std::byte{ 0 });
        CHECK(bytes[3] == std::byte{ 0 });
        before = callsOf(list);
        list.drawMeshTasks(2, 1, 1);
        CHECK(callsOf(list) - before == 1);
        list.endRenderPass();
        // A later pass of the same recording finds the record bound.
        list.beginRenderPass(passOf(colors));
        list.setPipeline(mesh);
        list.setViewport(kViewport);
        list.setScissor(kScissor);
        before = callsOf(list);
        list.drawMeshTasks(1, 1, 1);
        CHECK(callsOf(list) - before == openCalls(list) + kPassBegin + 5 + 1);
        CHECK(drawOf(list).zeroRecord == record);
        list.endRenderPass();
        list.end();
        device.queue(QueueType::Graphics).submit(std::array{ list });
    }
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
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

//======================================================================================================================
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
    CHECK(drawOf(list).zeroRecord == 0);
    list.endRenderPass();
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("C8's sequence makes the calls the overhead note registers", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
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
        // writeTimestampIntoHeap; renderCommandEncoder, setLabel, setArgumentTable; the consumer barrier after the first pass;
        // setRenderPipelineState, setFrontFacingWinding, setViewport, setScissorRect, drawPrimitives; writeTimestamp on the
        // encoder and popDebugGroup. The winding call, one per encoder, is beyond the registered sequence: the pipeline has a
        // fragment entry, which may read the front face, and its counter-clockwise winding is not Metal's default.
        const uint64_t barrier = repetition > 0 ? 1 : 0;
        CHECK(callsOf(list) - before == barrier + 2 + openCalls(list) + barrier + 5 + 2);
        CHECK(countsOf(list).nativeBarriers - barriers == barrier);
    }
    finish(device, list);
    destroyDevice(device);
}
