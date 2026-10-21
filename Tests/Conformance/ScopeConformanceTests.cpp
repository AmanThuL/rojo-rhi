//----------------------------------------------------------------------------------------------------------------------
/// @file ScopeConformanceTests.cpp
/// @brief Observes chapter 14's scope timings on a GPU: durations around GPU work, order, queue, and the scopes of passes and upscales.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Backend.h"
#include "Support/Conformance.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The frames in flight of every case's device.
    constexpr uint32_t kFramesInFlight = 2;

    /// The size of the fill each timed scope encloses: long enough to take measurable GPU time.
    constexpr uint64_t kBig = uint64_t{ 64 } << 20;

    /// A fill pattern whose bytes differ, so the fill runs as a kernel over the whole range.
    constexpr uint32_t kUnequal = 0x01020304u;

    //==================================================================================================================
    /// Ends `lists`, submits each to the queue of its type, joins the compute lists into the frame's graphics queue through
    /// a fence, and waits on the host for all of them.
    void submit(Device device, std::span<const CommandList> lists, std::span<const QueueType> types)
    {
        const Fence fence = *device.createFence(0, "test.done");
        uint64_t value = 0;
        for (size_t i = 0; i < lists.size(); ++i)
        {
            CommandList list = lists[i];
            list.end();
            const std::array<FenceValue, 1> signal{ FenceValue{ fence, ++value } };
            const std::array<FenceValue, 1> wait{ FenceValue{ fence, value - 1 } };
            device.queue(types[i]).submit(std::array{ lists[i] }, value > 1 ? std::span<const FenceValue>(wait) : std::span<const FenceValue>(), signal);
        }
        const std::array<FenceValue, 1> wait{ FenceValue{ fence, value } };
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, value + 1 } };
        device.queue(QueueType::Graphics).submit({}, wait, signal);
        REQUIRE(device.wait({ fence, value + 1 }, UINT64_MAX));
    }

    //==================================================================================================================
    /// Ends the open frame and begins frames until the one `beginFrame` waits for is the frame that was open, so
    /// `Device::scopeTimings` reports its scopes; a frame is open on return.
    void retireOpenFrame(Device device)
    {
        device.endFrame();
        for (uint32_t frame = 1; frame < kFramesInFlight; ++frame)
        {
            device.beginFrame();
            device.endFrame();
        }
        device.beginFrame();
    }

    /// The edge of the drawn target, in texels: large enough that drawing it takes measurable GPU time.
    constexpr uint32_t kDrawnEdge = 1024;

    /// The instances of the full-target triangle the timed pass draws, each covering every texel of the target.
    constexpr uint32_t kInstances = 64;

    /// The input and output extents of the upscaler, in texels.
    constexpr uint32_t kUpscaleInput = 64;
    constexpr uint32_t kUpscaleOutput = 128;

    /// Every subresource of a single-mip, single-layer texture.
    constexpr SubresourceRange kWhole{ .baseMip = 0, .mipCount = 1, .baseLayer = 0, .layerCount = 1 };

    //==================================================================================================================
    /// Creates the pipeline that draws `DrawOracle.slang`'s full-target triangle in a solid color over one `RGBA16Float`
    /// target, from `library`.
    Pipeline solidPipeline(Device device, ShaderLibrary library)
    {
        const std::array colors{ ColorTarget{ .format = Format::RGBA16Float } };
        return *device.createPipeline(
            GraphicsPipelineDesc{
                .vertex = { library, "fullscreenVertexMain" }, .fragment = { library, "solidFragmentMain" }, .colors = colors, .label = "test.solid" }
        );
    }

    //==================================================================================================================
    /// Creates a 2D texture of `format`, `edge` texels square, with `usage`.
    Texture square(Device device, Format format, uint32_t edge, TextureUsage usage, const char* label)
    {
        return *device.createTexture(TextureDesc{ .format = format, .extent = { edge, edge, 1 }, .usage = usage, .label = label });
    }

    //==================================================================================================================
    /// Records a barrier that moves the whole of `texture` from `oldLayout` to `newLayout`, from `srcStage` and `srcAccess` to
    /// `dstStage` and `dstAccess`.
    void transition(CommandList list, Texture texture, Layout oldLayout, Layout newLayout, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .range = kWhole,
                                                                      .srcStage = srcStage,
                                                                      .srcAccess = srcAccess,
                                                                      .dstStage = dstStage,
                                                                      .dstAccess = dstAccess,
                                                                      .oldLayout = oldLayout,
                                                                      .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Begins a render pass labelled `label` over the whole of `target`, an `RGBA16Float` texture `edge` texels square, cleared,
    /// with `pipeline` bound and the viewport and scissor of the whole target.
    void beginDrawing(CommandList list, Texture target, uint32_t edge, Pipeline pipeline, std::string_view label)
    {
        const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, edge, edge }, .label = label });
        list.setPipeline(pipeline);
        list.setViewport(Viewport{ .width = static_cast<float>(edge), .height = static_cast<float>(edge) });
        list.setScissor(Rect2D{ 0, 0, edge, edge });
    }

    //==================================================================================================================
    /// Records the barrier that orders one pass's writes to `target` before the next pass's.
    void betweenPasses(CommandList list, Texture target)
    {
        transition(
            list,
            target,
            Layout::ColorAttachment,
            Layout::ColorAttachment,
            Stage::ColorAttachment,
            Access::ColorAttachmentWrite,
            Stage::ColorAttachment,
            Access::ColorAttachmentRead | Access::ColorAttachmentWrite
        );
    }

    //==================================================================================================================
    /// Creates an `IndirectArgs` buffer of `count` draw records of the full-target triangle, `instances` instances each.
    Buffer drawRecords(Device device, uint32_t count, uint32_t instances)
    {
        const std::vector<DrawIndirectArgs> records(count, DrawIndirectArgs{ .drawId = 0, .vertexCount = 3, .instanceCount = instances });
        return *device.createBuffer(
            BufferDesc{ .size = records.size() * sizeof(DrawIndirectArgs), .usage = BufferUsage::IndirectArgs, .label = "test.records" },
            std::as_bytes(std::span(records))
        );
    }

    //==================================================================================================================
    /// Returns true when `timing` is finite and at least zero.
    bool finiteAndNonNegative(const ScopeTiming& timing)
    {
        return std::isfinite(timing.gpuMilliseconds) && timing.gpuMilliseconds >= 0.0;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a scope around GPU work reports a positive duration", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .scopeTimings = true, .framesInFlight = kFramesInFlight });
    const Buffer target = *device.createBuffer({ .size = kBig, .usage = BufferUsage::ShaderWrite, .label = "test.target" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.beginScope("test.fill");
    list.fillBuffer(target, 0, kBig, kUnequal);
    list.endScope();
    submit(device, std::array{ list }, std::array{ QueueType::Graphics });
    retireOpenFrame(device);
    // Chapter 14, `ScopeTiming`: the GPU wall time between the scope's two timestamps, which enclose the fill.
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 1);
    CHECK(report[0].label == "test.fill");
    CHECK(report[0].frame == 1);
    CHECK(std::isfinite(report[0].gpuMilliseconds));
    CHECK(report[0].gpuMilliseconds > 0.0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("nested scopes report parents before children with finite non-negative durations", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .scopeTimings = true, .framesInFlight = kFramesInFlight });
    const Buffer target = *device.createBuffer({ .size = kBig, .usage = BufferUsage::ShaderWrite, .label = "test.target" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.beginScope("test.outer");
    list.beginScope("test.first");
    list.fillBuffer(target, 0, kBig / 2, kUnequal);
    list.endScope();
    list.beginScope("test.second");
    list.beginScope("test.innermost");
    list.fillBuffer(target, kBig / 2, kBig / 2, kUnequal);
    list.endScope();
    list.endScope();
    list.endScope();
    submit(device, std::array{ list }, std::array{ QueueType::Graphics });
    retireOpenFrame(device);
    // Chapter 14, `Device::scopeTimings`: within a command list in the order the scopes began, a parent before its children.
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 4);
    const std::array<std::string_view, 4> labels{ "test.outer", "test.first", "test.second", "test.innermost" };
    const std::array<uint32_t, 4> depths{ 0, 1, 1, 2 };
    for (size_t i = 0; i < report.size(); ++i)
    {
        CAPTURE(i);
        CHECK(report[i].label == labels[i]);
        CHECK(report[i].depth == depths[i]);
        CHECK(finiteAndNonNegative(report[i]));
    }
    // Chapter 14, `ScopeTiming`: each duration is the wall time between the scope's two timestamps, and nested scopes
    // overlap their parent, so on one command buffer a parent's timestamps enclose its children's: no child outlasts it.
    CHECK(report[0].gpuMilliseconds > 0.0);
    CHECK(report[0].gpuMilliseconds >= report[1].gpuMilliseconds);
    CHECK(report[0].gpuMilliseconds >= report[2].gpuMilliseconds);
    CHECK(report[2].gpuMilliseconds >= report[3].gpuMilliseconds);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("scopes on the compute queue are timed and reported with their queue", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .scopeTimings = true, .framesInFlight = kFramesInFlight });
    const Buffer target = *device.createBuffer({ .size = kBig, .usage = BufferUsage::ShaderWrite, .label = "test.target" });
    device.beginFrame();
    std::array<CommandList, 2> lists{ device.acquireCommandList(QueueType::Graphics, "test.graphics"),
                                      device.acquireCommandList(QueueType::Compute, "test.compute") };
    lists[0].beginScope("test.graphics");
    lists[0].endScope();
    lists[1].beginScope("test.compute");
    lists[1].fillBuffer(target, 0, kBig, kUnequal);
    lists[1].endScope();
    submit(device, lists, std::array{ QueueType::Graphics, QueueType::Compute });
    retireOpenFrame(device);
    // Chapter 14: `queue` is the queue type of the list that recorded the scope; records are in submission order per queue.
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 2);
    const ScopeTiming* compute = nullptr;
    for (const ScopeTiming& timing : report)
    {
        CHECK(finiteAndNonNegative(timing));
        if (timing.label == "test.compute")
        {
            compute = &timing;
        }
    }
    REQUIRE(compute != nullptr);
    CHECK(compute->queue == QueueType::Compute);
    CHECK(compute->gpuMilliseconds > 0.0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an empty scope reports a finite duration of at least zero", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .scopeTimings = true, .framesInFlight = kFramesInFlight });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.beginScope("test.empty");
    list.endScope();
    submit(device, std::array{ list }, std::array{ QueueType::Graphics });
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 1);
    CHECK(report[0].label == "test.empty");
    CHECK(finiteAndNonNegative(report[0]));
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a render pass that draws reports a positive duration", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .scopeTimings = true, .framesInFlight = kFramesInFlight });
    const ShaderLibrary library = *device.loadShaderLibrary(test::shaderLibraryFile("DrawOracle"), "DrawOracle");
    const Pipeline solid = solidPipeline(device, library);
    const Texture target = square(device, Format::RGBA16Float, kDrawnEdge, TextureUsage::RenderTarget, "test.target");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    beginDrawing(list, target, kDrawnEdge, solid, "test.pass");
    list.draw(3, kInstances);
    list.endRenderPass();
    submit(device, std::array{ list }, std::array{ QueueType::Graphics });
    retireOpenFrame(device);
    // Chapter 14, `ScopeTiming`: every render pass forms its own scope, whose duration is the GPU wall time between its two
    // timestamps, which enclose the pass's draws.
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 1);
    CHECK(report[0].label == "test.pass");
    CHECK(report[0].frame == 1);
    CHECK(report[0].depth == 0);
    CHECK(std::isfinite(report[0].gpuMilliseconds));
    CHECK(report[0].gpuMilliseconds > 0.0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a pass, a scope inside it and an upscale are each reported with their label, depth and a finite duration", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .scopeTimings = true, .framesInFlight = kFramesInFlight });
    if (device.capabilities().upscalers.empty())
    {
        destroyDevice(device);
        SKIP("the device lists no upscaler");
    }
    const ShaderLibrary library = *device.loadShaderLibrary(test::shaderLibraryFile("DrawOracle"), "DrawOracle");
    const Pipeline solid = solidPipeline(device, library);
    const Upscaler upscaler = *device.createUpscaler(
        UpscalerDesc{ .input = { kUpscaleInput, kUpscaleInput }, .output = { kUpscaleOutput, kUpscaleOutput }, .label = "test.upscaler" }
    );
    constexpr TextureUsage kInput = TextureUsage::Sampled | TextureUsage::RenderTarget;
    const Texture color = square(device, Format::RGBA16Float, kUpscaleInput, kInput, "test.color");
    const Texture depth = square(device, Format::D32Float, kUpscaleInput, TextureUsage::Sampled | TextureUsage::DepthStencil, "test.depth");
    const Texture motion = square(device, Format::RG16Float, kUpscaleInput, kInput, "test.motion");
    const Texture output = square(device, Format::RGBA16Float, kUpscaleOutput, TextureUsage::Storage | TextureUsage::RenderTarget, "test.output");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    // The depth and motion inputs hold uniform values, the color input is what the pass draws, and the output is written whole.
    const auto clearInput = [&](Texture input, const ClearValue& value) {
        transition(list, input, Layout::Undefined, Layout::CopyDst, Stage::None, Access::None, Stage::Copy, Access::CopyWrite);
        list.clearTexture(input, kWhole, value);
        transition(list, input, Layout::CopyDst, Layout::ShaderRead, Stage::Copy, Access::CopyWrite, Stage::ComputeShader, Access::ShaderRead);
    };
    clearInput(depth, ClearValue{ .depth = 0.5f });
    clearInput(motion, ClearValue{});
    transition(list, color, Layout::Undefined, Layout::ColorAttachment, Stage::None, Access::None, Stage::ColorAttachment, Access::ColorAttachmentWrite);
    transition(list, output, Layout::Undefined, Layout::ShaderWrite, Stage::None, Access::None, Stage::ComputeShader, Access::ShaderWrite);
    beginDrawing(list, color, kUpscaleInput, solid, "test.pass");
    list.beginScope("test.inner");
    list.draw(3);
    list.endScope();
    list.endRenderPass();
    transition(
        list, color, Layout::ColorAttachment, Layout::ShaderRead, Stage::ColorAttachment, Access::ColorAttachmentWrite, Stage::ComputeShader, Access::ShaderRead
    );
    list.upscale(upscaler, UpscaleParams{ .color = color, .depth = depth, .motion = motion, .output = output, .reset = true, .label = "test.upscale" });
    submit(device, std::array{ list }, std::array{ QueueType::Graphics });
    retireOpenFrame(device);
    // Chapter 14: every render pass and every upscale forms its own scope; within a command list the records are in the order
    // the scopes began, a parent before its children, and a scope inside a pass is nested in the pass's.
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 3);
    const std::array<std::string_view, 3> labels{ "test.pass", "test.inner", "test.upscale" };
    const std::array<uint32_t, 3> depths{ 0, 1, 0 };
    for (size_t i = 0; i < report.size(); ++i)
    {
        CAPTURE(i);
        CHECK(report[i].label == labels[i]);
        CHECK(report[i].depth == depths[i]);
        CHECK(report[i].frame == 1);
        CHECK(report[i].queue == QueueType::Graphics);
        CHECK(finiteAndNonNegative(report[i]));
    }
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a render pass that draws reports more than an empty pass", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .scopeTimings = true, .framesInFlight = kFramesInFlight });
    const ShaderLibrary library = *device.loadShaderLibrary(test::shaderLibraryFile("DrawOracle"), "DrawOracle");
    const Pipeline solid = solidPipeline(device, library);
    const Texture target = square(device, Format::RGBA16Float, kDrawnEdge, TextureUsage::RenderTarget, "test.target");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    beginDrawing(list, target, kDrawnEdge, solid, "test.empty");
    list.endRenderPass();
    betweenPasses(list, target);
    beginDrawing(list, target, kDrawnEdge, solid, "test.drawn");
    list.draw(3, kInstances);
    list.endRenderPass();
    submit(device, std::array{ list }, std::array{ QueueType::Graphics });
    retireOpenFrame(device);
    // Chapter 14, `ScopeTiming`: a pass's duration is the GPU wall time between its two timestamps, which enclose its work,
    // so a pass that draws the target whole many times over takes longer than one that only clears and stores it.
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 2);
    CHECK(report[0].label == "test.empty");
    CHECK(report[1].label == "test.drawn");
    CHECK(finiteAndNonNegative(report[0]));
    CHECK(std::isfinite(report[1].gpuMilliseconds));
    CHECK(report[1].gpuMilliseconds > report[0].gpuMilliseconds);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a render pass of counted draws only reports a positive duration, more than an empty pass", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .scopeTimings = true, .framesInFlight = kFramesInFlight });
    const ShaderLibrary library = *device.loadShaderLibrary(test::shaderLibraryFile("DrawOracle"), "DrawOracle");
    const Pipeline solid = solidPipeline(device, library);
    const Texture target = square(device, Format::RGBA16Float, kDrawnEdge, TextureUsage::RenderTarget, "test.target");
    constexpr uint32_t kRecords = 4;
    const Buffer records = drawRecords(device, kRecords, kInstances / kRecords);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    beginDrawing(list, target, kDrawnEdge, solid, "test.empty");
    list.endRenderPass();
    betweenPasses(list, target);
    beginDrawing(list, target, kDrawnEdge, solid, "test.counted");
    list.drawIndirect(device.address(records), kRecords);
    list.endRenderPass();
    submit(device, std::array{ list }, std::array{ QueueType::Graphics });
    retireOpenFrame(device);
    // Chapter 14, `ScopeTiming`: the pass's timestamps enclose all its work, its counted draws included, however the backend
    // lowers them.
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 2);
    CHECK(report[0].label == "test.empty");
    CHECK(report[1].label == "test.counted");
    CHECK(finiteAndNonNegative(report[0]));
    CHECK(std::isfinite(report[1].gpuMilliseconds));
    CHECK(report[1].gpuMilliseconds > 0.0);
    CHECK(report[1].gpuMilliseconds > report[0].gpuMilliseconds);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a scope inside a render pass reports no more than its pass", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .scopeTimings = true, .framesInFlight = kFramesInFlight });
    const ShaderLibrary library = *device.loadShaderLibrary(test::shaderLibraryFile("DrawOracle"), "DrawOracle");
    const Pipeline solid = solidPipeline(device, library);
    const Texture target = square(device, Format::RGBA16Float, kDrawnEdge, TextureUsage::RenderTarget, "test.target");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    beginDrawing(list, target, kDrawnEdge, solid, "test.pass");
    list.draw(3, kInstances);
    list.beginScope("test.inner");
    list.draw(3, kInstances);
    list.endScope();
    list.draw(3, kInstances);
    list.endRenderPass();
    submit(device, std::array{ list }, std::array{ QueueType::Graphics });
    retireOpenFrame(device);
    // Chapter 14, `ScopeTiming`: nested scopes overlap their parent, and a scope inside a pass is nested in the pass's, so on
    // one command buffer the pass's timestamps enclose the inner scope's.
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 2);
    CHECK(report[0].label == "test.pass");
    CHECK(report[1].label == "test.inner");
    CHECK(report[1].depth == 1);
    CHECK(finiteAndNonNegative(report[1]));
    CHECK(std::isfinite(report[0].gpuMilliseconds));
    CHECK(report[0].gpuMilliseconds > 0.0);
    CHECK(report[1].gpuMilliseconds <= report[0].gpuMilliseconds);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a render pass a counted draw splits reports one duration, more than the pass without the split", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .scopeTimings = true, .framesInFlight = kFramesInFlight });
    const ShaderLibrary library = *device.loadShaderLibrary(test::shaderLibraryFile("DrawOracle"), "DrawOracle");
    const Pipeline solid = solidPipeline(device, library);
    const Texture target = square(device, Format::RGBA16Float, kDrawnEdge, TextureUsage::RenderTarget, "test.target");
    const Buffer records = drawRecords(device, 1, 1);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    beginDrawing(list, target, kDrawnEdge, solid, "test.single");
    list.draw(3, kInstances);
    list.endRenderPass();
    betweenPasses(list, target);
    // A counted draw after a direct one splits the pass on Metal 4 (chapter 08); the second direct draw follows it.
    beginDrawing(list, target, kDrawnEdge, solid, "test.split");
    list.draw(3, kInstances);
    list.drawIndirect(device.address(records), 1);
    list.draw(3, kInstances);
    list.endRenderPass();
    submit(device, std::array{ list }, std::array{ QueueType::Graphics });
    retireOpenFrame(device);
    // Chapter 14: every render pass forms its own scope, one record whatever its lowering, and its timestamps enclose all
    // its work: the pass that draws the target twice as often takes longer than the one that draws it once.
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 2);
    CHECK(report[0].label == "test.single");
    CHECK(report[1].label == "test.split");
    CHECK(std::isfinite(report[0].gpuMilliseconds));
    CHECK(report[0].gpuMilliseconds > 0.0);
    CHECK(std::isfinite(report[1].gpuMilliseconds));
    CHECK(report[1].gpuMilliseconds > report[0].gpuMilliseconds);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a device created without scopeTimings reports no scope of a frame that ran scopes and passes", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .framesInFlight = kFramesInFlight });
    const ShaderLibrary library = *device.loadShaderLibrary(test::shaderLibraryFile("DrawOracle"), "DrawOracle");
    const Pipeline solid = solidPipeline(device, library);
    const Texture target = square(device, Format::RGBA16Float, kDrawnEdge, TextureUsage::RenderTarget, "test.target");
    const Buffer filled = *device.createBuffer({ .size = kBig, .usage = BufferUsage::ShaderWrite, .label = "test.filled" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.beginScope("test.fill");
    list.fillBuffer(filled, 0, kBig, kUnequal);
    list.endScope();
    beginDrawing(list, target, kDrawnEdge, solid, "test.pass");
    list.beginScope("test.inner");
    list.draw(3, kInstances);
    list.endScope();
    list.endRenderPass();
    submit(device, std::array{ list }, std::array{ QueueType::Graphics });
    retireOpenFrame(device);
    // Chapter 14, `Device::scopeTimings`: a device created without `DeviceDesc::scopeTimings` writes no timestamp, so the
    // frame its beginFrame waited for reports no scope although it ran GPU work in scopes and a pass.
    CHECK(device.scopeTimings().empty());
    device.endFrame();
    destroyDevice(device);
}
