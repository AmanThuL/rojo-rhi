//----------------------------------------------------------------------------------------------------------------------
/// @file DiagnosticsTests.cpp
/// @brief Tests access declarations, scope timings and capture against chapter 14.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/PassKit.h"
#include "Support/UpscaleKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace rojoRHI;
using rojoRHI::test::PassKit;
using rojoRHI::test::UpscaleKit;

namespace {
    /// A scope timing with its label copied, so it outlives the next `beginFrame`.
    struct Seen
    {
        std::string label;
        uint64_t frame;
        QueueType queue;
        uint32_t depth;
        double gpuMilliseconds;
    };

    //==================================================================================================================
    std::vector<Seen> seenBy(Device device)
    {
        std::vector<Seen> seen;
        for (const ScopeTiming& timing : device.scopeTimings())
        {
            seen.push_back({ std::string(timing.label), timing.frame, timing.queue, timing.depth, timing.gpuMilliseconds });
        }
        return seen;
    }

    //==================================================================================================================
    void submit(Device device, CommandList list, QueueType type = QueueType::Graphics)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists);
    }

    //==================================================================================================================
    /// Runs `frames` empty frames.
    void idleFrames(Device device, uint32_t frames)
    {
        for (uint32_t i = 0; i < frames; ++i)
        {
            device.beginFrame();
            device.endFrame();
        }
    }

    //==================================================================================================================
    ResourceAccess fragmentRead(Buffer buffer)
    {
        return ResourceAccess{ .buffer = buffer, .stage = Stage::FragmentShader, .access = Access::ShaderRead };
    }
} // namespace

//======================================================================================================================
TEST_CASE("declareAccess before a draw is accepted on a device without validation", "[contract]")
{
    PassKit kit;
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    kit.readyToDraw();
    const std::array<ResourceAccess, 1> accesses{ fragmentRead(buffer) };
    kit.list.declareAccess(accesses);
    kit.list.draw(3);
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("declareAccess calls accumulate and the next draw consumes them", "[contract]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage);
    // The declared write is judged at submission against the texture's layout and the barrier before it.
    test::transition(kit.list, texture, Layout::Undefined, Layout::ShaderWrite);
    kit.readyToDraw();
    const std::array<ResourceAccess, 1> first{ fragmentRead(buffer) };
    const std::array<ResourceAccess, 1> second{ ResourceAccess{
        .texture = texture, .stage = Stage::VertexShader, .access = Access::ShaderRead | Access::ShaderWrite } };
    kit.list.declareAccess(first);
    kit.list.declareAccess(second);
    kit.list.draw(3);
    kit.list.draw(3);
    kit.list.endRenderPass();
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("declareAccess names a compute stage for a dispatch and an empty span is a no-op", "[contract]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    kit.list.setPipeline(kit.compute());
    kit.list.declareAccess({});
    const std::array<ResourceAccess, 1> accesses{ ResourceAccess{
        .buffer = buffer, .offset = 256, .size = 512, .stage = Stage::ComputeShader, .access = Access::ShaderWrite } };
    kit.list.declareAccess(accesses);
    kit.list.dispatch(1, 1, 1);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("scopeTimings is empty until a frame has retired", "[contract]")
{
    Device device = *createDevice(DeviceDesc{ .scopeTimings = true, .framesInFlight = 2 });
    CHECK(device.scopeTimings().empty());
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.beginScope("scope");
    list.endScope();
    submit(device, list);
    device.endFrame();
    device.beginFrame();
    CHECK(device.scopeTimings().empty());
    device.endFrame();
    device.beginFrame();
    CHECK(device.scopeTimings().size() == 1);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("scopeTimings reports the scopes of the frame beginFrame waited for, parents first", "[contract]")
{
    Device device = *createDevice(DeviceDesc{ .scopeTimings = true, .framesInFlight = 2 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.beginScope("outer");
    list.beginScope("inner");
    list.endScope();
    list.endScope();
    list.beginScope("second");
    list.endScope();
    submit(device, list);
    device.endFrame();
    idleFrames(device, 1);
    device.beginFrame();
    const std::vector<Seen> seen = seenBy(device);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0].label == "outer");
    CHECK(seen[0].depth == 0);
    CHECK(seen[1].label == "inner");
    CHECK(seen[1].depth == 1);
    CHECK(seen[2].label == "second");
    CHECK(seen[2].depth == 0);
    for (const Seen& scope : seen)
    {
        CHECK(scope.frame == 1);
        CHECK(scope.queue == QueueType::Graphics);
        CHECK(scope.gpuMilliseconds >= 0.0);
    }
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("scopeTimings is empty again for the next retired frame when it had no scopes", "[contract]")
{
    Device device = *createDevice(DeviceDesc{ .scopeTimings = true, .framesInFlight = 2 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.beginScope("scope");
    list.endScope();
    submit(device, list);
    device.endFrame();
    idleFrames(device, 1);
    device.beginFrame();
    CHECK(device.scopeTimings().size() == 1);
    device.endFrame();
    device.beginFrame();
    CHECK(device.scopeTimings().empty());
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("every render pass forms a scope labeled with the pass label, defaulted when empty", "[contract]")
{
    Device device = *createDevice(DeviceDesc{ .scopeTimings = true, .framesInFlight = 2 });
    const Texture target =
        *device.createTexture(TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    // A new texture is Undefined, which a pass cannot render to where layouts are native (chapter 09).
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = target } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 8, 8 }, .label = "named.pass" });
    list.beginScope("inside");
    list.endScope();
    list.endRenderPass();
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 8, 8 } });
    list.endRenderPass();
    submit(device, list);
    device.endFrame();
    idleFrames(device, 1);
    device.beginFrame();
    const std::vector<Seen> seen = seenBy(device);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0].label == "named.pass");
    CHECK(seen[0].depth == 0);
    CHECK(seen[1].label == "inside");
    CHECK(seen[1].depth == 1);
    CHECK(seen[2].label == "rojorhi.pass.unnamed");
    CHECK(seen[2].depth == 0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an empty scope label is replaced by the scope default", "[contract]")
{
    Device device = *createDevice(DeviceDesc{ .scopeTimings = true, .framesInFlight = 2 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.beginScope("");
    list.endScope();
    submit(device, list);
    device.endFrame();
    idleFrames(device, 1);
    device.beginFrame();
    const std::vector<Seen> seen = seenBy(device);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].label == "rojorhi.scope.unnamed");
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an upscale forms a scope named params.label, defaulted to rojorhi.upscale.unnamed", "[contract]")
{
    test::requireUpscaleCompletion();
    UpscaleKit kit(QueueType::Graphics, UpscaleKit::defaultDesc(), false, true);
    Device device = kit.kit.device;
    kit.params.label = "named.upscale";
    kit.upscale();
    kit.nextFrame();
    kit.params.label = {};
    kit.upscale();
    kit.nextFrame();
    kit.nextFrame();
    // Frame 4 is open: its beginFrame waited for frame 1.
    std::vector<Seen> seen = seenBy(device);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].label == "named.upscale");
    CHECK(seen[0].frame == 1);
    CHECK(seen[0].depth == 0);
    kit.nextFrame();
    seen = seenBy(device);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].label == "rojorhi.upscale.unnamed");
    CHECK(seen[0].frame == 2);
}

//======================================================================================================================
TEST_CASE("a device created without scopeTimings reports no scope after frames with scopes, passes and upscales", "[contract]")
{
    test::requireUpscaleCompletion();
    UpscaleKit kit;
    Device device = kit.kit.device;
    for (uint32_t frame = 0; frame < 4; ++frame)
    {
        kit.kit.list.beginScope("outer");
        kit.kit.list.beginScope("inner");
        kit.kit.list.endScope();
        kit.kit.list.endScope();
        kit.kit.beginSimplePass();
        kit.kit.list.beginScope("inside");
        kit.kit.list.endScope();
        kit.kit.list.endRenderPass();
        kit.upscale();
        kit.nextFrame();
        // From the third iteration on, this beginFrame waited for a frame that recorded scopes, passes and an upscale.
        CHECK(device.scopeTimings().empty());
    }
}

//======================================================================================================================
TEST_CASE("beginCapture with a path either starts a capture or returns an error, and endCapture ends one", "[contract]")
{
    Device device = *createDevice();
    const Result<void> started = device.beginCapture("capture.trace");
    if (started.has_value())
    {
        device.endCapture();
    }
    destroyDevice(device);
}
