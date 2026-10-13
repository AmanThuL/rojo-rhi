//----------------------------------------------------------------------------------------------------------------------
/// @file DiagnosticsCoreTests.cpp
/// @brief Tests the state the core keeps for declared accesses, recorded scopes and capture, and the capture rule only a hook reaches.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;
using rojoRHI::test::PassKit;

namespace {
    //==================================================================================================================
    Result<void> captureAvailable(std::string_view)
    {
        return {};
    }

    //==================================================================================================================
    Result<void> captureRefused(std::string_view)
    {
        return std::unexpected(Error{ ErrorCategory::Unsupported, "no capture tool" });
    }

    //==================================================================================================================
    void submit(Device device, CommandList list)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(QueueType::Graphics).submit(lists);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a declaration on a validating device waits in the list's core until the next draw", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    kit.readyToDraw();
    const std::array<ResourceAccess, 2> accesses{ ResourceAccess{ .buffer = buffer, .stage = Stage::FragmentShader, .access = Access::ShaderRead },
                                                  ResourceAccess{ .buffer = buffer, .stage = Stage::VertexShader, .access = Access::ShaderWrite } };
    kit.list.declareAccess(accesses);
    kit.list.declareAccess(std::span(accesses).first(1));
    CHECK(core::coreOf(kit.list).pendingAccesses.size() == 3);
    kit.list.draw(3);
    CHECK(core::coreOf(kit.list).pendingAccesses.empty());
    kit.list.endRenderPass();
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a device without validation keeps no declaration", "[core]")
{
    PassKit kit;
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const std::array<ResourceAccess, 1> accesses{ ResourceAccess{ .buffer = buffer, .stage = Stage::FragmentShader, .access = Access::ShaderRead } };
    kit.list.declareAccess(accesses);
    CHECK(core::coreOf(kit.list).pendingAccesses.empty());
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a declaration no longer waits once the list is acquired again", "[core]")
{
#ifndef NDEBUG
    Device device = *createDevice(DeviceDesc{ .validation = true, .framesInFlight = 2 });
    const Buffer buffer = *device.createBuffer(BufferDesc{ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "core.buffer" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "core.list");
    const std::array<ResourceAccess, 1> accesses{ ResourceAccess{ .buffer = buffer, .stage = Stage::ComputeShader, .access = Access::ShaderRead } };
    list.declareAccess(accesses);
    CHECK(core::coreOf(list).pendingAccesses.size() == 1);
    core::coreOf(list).pendingAccesses.clear();
    submit(device, list);
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    const CommandList again = device.acquireCommandList(QueueType::Graphics, "core.list");
    CHECK(core::coreOf(again).pendingAccesses.empty());
    submit(device, again);
    device.endFrame();
    destroyDevice(device);
#endif
}

//======================================================================================================================
TEST_CASE("a scope's label is copied while recording", "[core]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "core.list");
    std::string label = "temporary.label";
    list.beginScope(label);
    label.assign(label.size(), 'x');
    list.endScope();
    const core::CommandListCore& state = core::coreOf(list);
    REQUIRE(state.scopeLog.size() == 1);
    CHECK(state.scopeLog[0].label == "temporary.label");
    CHECK(state.scopeLog[0].depth == 0);
    submit(device, list);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the reported labels live in device storage that outlives the frame arena until the next beginFrame", "[core]")
{
    Device device = *createDevice(DeviceDesc{ .framesInFlight = 2 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "core.list");
    list.beginScope("kept.label");
    list.endScope();
    submit(device, list);
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    const std::span<const ScopeTiming> timings = device.scopeTimings();
    REQUIRE(timings.size() == 1);
    // The slot's arena pages were rewound by this beginFrame; claiming one here overwrites what a label kept in them.
    CommandList other = device.acquireCommandList(QueueType::Graphics, "core.other");
    const std::array<std::byte, 64> filler = {};
    (void)other.uploadFrameData(filler);
    other.beginScope("overwriting.label.of.another.frame");
    other.endScope();
    CHECK(timings[0].label == "kept.label");
    submit(device, other);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a scope recorded on a list of each queue type is reported with that queue", "[core]")
{
    Device device = *createDevice(DeviceDesc{ .framesInFlight = 2 });
    device.beginFrame();
    for (const QueueType type : { QueueType::Copy, QueueType::Compute, QueueType::Graphics })
    {
        CommandList list = device.acquireCommandList(type, "core.list");
        list.beginScope("scope");
        list.endScope();
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists);
    }
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    const std::span<const ScopeTiming> timings = device.scopeTimings();
    REQUIRE(timings.size() == 3);
    CHECK(timings[0].queue == QueueType::Graphics);
    CHECK(timings[1].queue == QueueType::Compute);
    CHECK(timings[2].queue == QueueType::Copy);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a capture the hook accepts is in progress until endCapture", "[core]")
{
    Device device = *createDevice();
    core::coreOf(device).setCaptureAvailable(captureAvailable);
    REQUIRE(device.beginCapture("capture.trace").has_value());
    CHECK(core::coreOf(device).capturing());
    device.endCapture();
    CHECK_FALSE(core::coreOf(device).capturing());
    CHECK(device.beginCapture("capture.trace").has_value());
    device.endCapture();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a capture the hook refuses returns its error and starts nothing", "[core]")
{
    Device device = *createDevice();
    core::coreOf(device).setCaptureAvailable(captureRefused);
    const Result<void> first = device.beginCapture("capture.trace");
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().category == ErrorCategory::Unsupported);
    CHECK_FALSE(core::coreOf(device).capturing());
    CHECK_FALSE(device.beginCapture("capture.trace").has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("without a hook beginCapture is unsupported", "[core]")
{
    Device device = *createDevice();
    const Result<void> result = device.beginCapture("capture.trace");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().category == ErrorCategory::Unsupported);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0008 a capture starts while this device's capture is in progress", "[core][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0008", [] {
        Device device = *createDevice();
        core::coreOf(device).setCaptureAvailable(captureAvailable);
        (void)device.beginCapture("capture.trace");
        (void)device.beginCapture("capture.trace");
    });
}
