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
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
    void submit(Device device, CommandList list, QueueType type = QueueType::Graphics)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists);
    }

    /// One call of a scope-time hook: the slot, and the labels and timestamps of the records and the report entries it saw.
    struct ScopeTimesCall
    {
        uint32_t slot = 0;
        std::vector<std::string> recordLabels;
        std::vector<uint32_t> timestamps;
        std::vector<std::string> reportLabels;
        std::vector<QueueType> reportQueues;
    };

    /// The calls the hook below saw, held by the live `ScopeTimesRecorder`; null when none is live.
    std::vector<ScopeTimesCall>* scopeTimesCalls = nullptr;

    /// Collects the calls of `recordScopeTimes` for its lifetime, so a case that ends early leaves no dangling pointer.
    struct ScopeTimesRecorder
    {
        std::vector<ScopeTimesCall> calls; ///< The calls seen, in order.

        /// Points `scopeTimesCalls` at `calls`.
        ScopeTimesRecorder();
        /// Resets `scopeTimesCalls` to null.
        ~ScopeTimesRecorder();
    };

    //==================================================================================================================
    ScopeTimesRecorder::ScopeTimesRecorder()
    {
        scopeTimesCalls = &calls;
    }

    //==================================================================================================================
    ScopeTimesRecorder::~ScopeTimesRecorder()
    {
        scopeTimesCalls = nullptr;
    }

    //==================================================================================================================
    /// Records the call and writes each record's timestamp plus a half as its duration, so each duration is distinct.
    void recordScopeTimes(void*, uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report)
    {
        ScopeTimesCall call{ .slot = slot };
        for (size_t index = 0; index < records.size(); ++index)
        {
            call.recordLabels.emplace_back(records[index].label);
            call.timestamps.push_back(records[index].timestamp);
            call.reportLabels.emplace_back(report[index].label);
            call.reportQueues.push_back(report[index].queue);
            report[index].gpuMilliseconds = static_cast<double>(records[index].timestamp) + 0.5;
        }
        scopeTimesCalls->push_back(std::move(call));
    }

    //==================================================================================================================
    /// Adds `recordScopeTimes` to the hooks the backend registered on `device`; the backend's context stays.
    void hookScopeTimes(Device device)
    {
        core::RetireHooks hooks = core::coreOf(device).retireHooks();
        hooks.scopeTimes = recordScopeTimes;
        core::coreOf(device).setRetireHooks(hooks);
    }

    //==================================================================================================================
    /// Opens and closes a scope named `label` on `list` and gives its record `timestamp`, as a backend that times it does.
    void timedScope(CommandList list, std::string_view label, uint32_t timestamp)
    {
        list.beginScope(label);
        core::coreOf(list).scopeLog.back().timestamp = timestamp;
        list.endScope();
    }

    //==================================================================================================================
    /// Retires frame 1 of a two-slot device: runs frame 2 and begins frame 3.
    void retireFirstFrame(Device device)
    {
        device.beginFrame();
        device.endFrame();
        device.beginFrame();
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
    Device device = *createDevice(DeviceDesc{ .scopeTimings = true, .framesInFlight = 2 });
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
    Device device = *createDevice(DeviceDesc{ .scopeTimings = true, .framesInFlight = 2 });
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
TEST_CASE("a scope record carries no timestamp until a backend sets one", "[core]")
{
    CHECK(core::ScopeRecord{}.timestamp == core::kNoTimestamp);
    CHECK(core::kNoTimestamp == UINT32_MAX);
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "core.list");
    list.beginScope("scope");
    list.endScope();
    REQUIRE(core::coreOf(list).scopeLog.size() == 1);
    CHECK(core::coreOf(list).scopeLog[0].timestamp == core::kNoTimestamp);
    submit(device, list);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the scope-time hook sees each queue's records with the report entries made for them, in order", "[core]")
{
    const ScopeTimesRecorder recorder;
    const std::vector<ScopeTimesCall>& calls = recorder.calls;
    Device device = *createDevice(DeviceDesc{ .scopeTimings = true, .framesInFlight = 2 });
    hookScopeTimes(device);
    device.beginFrame();
    // The compute list is submitted first; the report still puts Graphics first.
    CommandList compute = device.acquireCommandList(QueueType::Compute, "core.compute");
    timedScope(compute, "compute.only", 4);
    submit(device, compute, QueueType::Compute);
    CommandList graphics = device.acquireCommandList(QueueType::Graphics, "core.graphics");
    graphics.beginScope("graphics.outer");
    core::coreOf(graphics).scopeLog.back().timestamp = 0;
    timedScope(graphics, "graphics.inner", 2);
    graphics.endScope();
    submit(device, graphics);
    device.endFrame();
    retireFirstFrame(device);

    REQUIRE(calls.size() == 2);
    CHECK(calls[0].slot == 1);
    CHECK(calls[0].recordLabels == std::vector<std::string>{ "graphics.outer", "graphics.inner" });
    CHECK(calls[0].timestamps == std::vector<uint32_t>{ 0, 2 });
    CHECK(calls[0].reportLabels == calls[0].recordLabels);
    CHECK(calls[0].reportQueues == std::vector<QueueType>{ QueueType::Graphics, QueueType::Graphics });
    CHECK(calls[1].slot == 1);
    CHECK(calls[1].recordLabels == std::vector<std::string>{ "compute.only" });
    CHECK(calls[1].reportLabels == calls[1].recordLabels);
    CHECK(calls[1].reportQueues == std::vector<QueueType>{ QueueType::Compute });

    const std::span<const ScopeTiming> timings = device.scopeTimings();
    REQUIRE(timings.size() == 3);
    CHECK(timings[0].label == "graphics.outer");
    CHECK(timings[0].gpuMilliseconds == 0.5);
    CHECK(timings[1].label == "graphics.inner");
    CHECK(timings[1].depth == 1);
    CHECK(timings[1].gpuMilliseconds == 2.5);
    CHECK(timings[2].label == "compute.only");
    CHECK(timings[2].gpuMilliseconds == 4.5);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("without a scope-time hook every duration is zero", "[core]")
{
    core::ScopeLog log(2);
    const std::array<core::ScopeRecord, 2> records{ core::ScopeRecord{ .label = "first", .depth = 0, .timestamp = 0 },
                                                    core::ScopeRecord{ .label = "second", .depth = 1, .timestamp = 2 } };
    log.submit(1, QueueType::Graphics, records);
    log.retire(1, 7, nullptr, nullptr);
    const std::span<const ScopeTiming> report = log.report();
    REQUIRE(report.size() == 2);
    CHECK(report[0].gpuMilliseconds == 0.0);
    CHECK(report[1].gpuMilliseconds == 0.0);
    CHECK(report[1].frame == 7);
}

//======================================================================================================================
TEST_CASE("a frame with scopes on no queue calls no scope-time hook", "[core]")
{
    const ScopeTimesRecorder recorder;
    const std::vector<ScopeTimesCall>& calls = recorder.calls;
    core::ScopeLog log(2);
    log.retire(0, 3, recordScopeTimes, nullptr);
    CHECK(calls.empty());
    const std::array<core::ScopeRecord, 1> records{ core::ScopeRecord{ .label = "copy.only", .timestamp = 6 } };
    log.submit(0, QueueType::Copy, records);
    log.clearReport();
    log.retire(0, 5, recordScopeTimes, nullptr);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].reportQueues == std::vector<QueueType>{ QueueType::Copy });
    REQUIRE(log.report().size() == 1);
    CHECK(log.report()[0].gpuMilliseconds == 6.5);
}

//======================================================================================================================
TEST_CASE("the hook runs before the slot's arena is rewound", "[core]")
{
    const ScopeTimesRecorder recorder;
    const std::vector<ScopeTimesCall>& calls = recorder.calls;
    Device device = *createDevice(DeviceDesc{ .scopeTimings = true, .framesInFlight = 2 });
    hookScopeTimes(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "core.list");
    timedScope(list, "label.in.the.arena", 0);
    // The label's view points into the slot's arena page, which the rewind of frame 3 makes claimable again.
    const char* const kept = core::coreOf(list).scopeLog.back().label.data();
    CHECK(kept != std::string_view("label.in.the.arena").data());
    submit(device, list);
    device.endFrame();
    retireFirstFrame(device);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].recordLabels == std::vector<std::string>{ "label.in.the.arena" });
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a device without scope timings still records its lists' scopes but calls no scope-time hook", "[core]")
{
    const ScopeTimesRecorder recorder;
    const std::vector<ScopeTimesCall>& calls = recorder.calls;
    Device device = *createDevice(DeviceDesc{ .framesInFlight = 2 });
    hookScopeTimes(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "core.list");
    list.beginScope("untimed.label");
    // The backend names its debug group after the record, so the list keeps it with its label.
    REQUIRE(core::coreOf(list).scopeLog.size() == 1);
    CHECK(core::coreOf(list).scopeLog.back().label == "untimed.label");
    CHECK(core::coreOf(list).scopeLog.back().timestamp == core::kNoTimestamp);
    list.endScope();
    submit(device, list);
    device.endFrame();
    retireFirstFrame(device);
    CHECK(calls.empty());
    CHECK(device.scopeTimings().empty());
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
