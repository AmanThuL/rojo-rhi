//----------------------------------------------------------------------------------------------------------------------
/// @file NullDiagnosticsTests.cpp
/// @brief Tests what only the Null backend promises about diagnostics: zero durations, queue order and no capture tool.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("Null reports every scope with a duration of zero", "[null]")
{
    Device device = *createDevice(DeviceDesc{ .scopeTimings = true, .framesInFlight = 2 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "null.list");
    list.beginScope("scope");
    list.endScope();
    list.end();
    const std::array<CommandList, 1> lists{ list };
    device.queue(QueueType::Graphics).submit(lists);
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    REQUIRE(device.scopeTimings().size() == 1);
    CHECK(device.scopeTimings()[0].gpuMilliseconds == 0.0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Null has no capture tool: beginCapture is Unsupported and starts nothing", "[null]")
{
    Device device = *createDevice();
    const Result<void> first = device.beginCapture("capture.gputrace");
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().category == ErrorCategory::Unsupported);
    // Nothing started, so a second call is an error result again, not a misuse.
    CHECK_FALSE(device.beginCapture("capture.gputrace").has_value());
    destroyDevice(device);
}
