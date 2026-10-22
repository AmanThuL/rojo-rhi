//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12SubmitTimingTests.cpp
/// @brief Tests the D3D12 backend's submission timing seam: one report per submission with lists, its times and its costs.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "D3D12Device.h"
#include "D3D12SubmitTiming.h"
#include "SubmitTiming.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The frames in flight of every case's device.
    constexpr uint32_t kFramesInFlight = 2;

    /// The bytes each list fills: long enough to take measurable GPU time.
    constexpr uint64_t kRegion = uint64_t{ 1 } << 20;

    /// A fill pattern whose bytes differ.
    constexpr uint32_t kPattern = 0x01020304u;

    /// How far, in seconds, a span may reach outside the CPU's window around it: the calibration's error, which is far
    /// below a millisecond on hardware.
    constexpr double kWindowSlack = 0.01;

    /// How far, in seconds, a list that waited for another queue's list may appear to start before that list ended: the
    /// difference of the two queues' calibrations.
    constexpr double kQueueSlack = 0.005;

    /// The reports a callback received and the thread each arrived on.
    struct Recorder
    {
        std::vector<d3d12::SubmitTime> times;
        std::vector<std::thread::id> threads;
    };

    //==================================================================================================================
    /// The seam's callback: appends `time` and the calling thread to the `Recorder` at `user`.
    void record(void* user, const d3d12::SubmitTime& time)
    {
        auto& recorder = *static_cast<Recorder*>(user);
        recorder.times.push_back(time);
        recorder.threads.push_back(std::this_thread::get_id());
    }

    //==================================================================================================================
    /// Returns the CPU's QueryPerformanceCounter in seconds, the time base of the seam's reports.
    double cpuSeconds()
    {
        LARGE_INTEGER ticks = {};
        LARGE_INTEGER frequency = {};
        QueryPerformanceCounter(&ticks);
        QueryPerformanceFrequency(&frequency);
        return static_cast<double>(ticks.QuadPart) / static_cast<double>(frequency.QuadPart);
    }

    //==================================================================================================================
    /// Returns a list of `type` acquired on `device` in the open frame that fills region `region` of `target`, ended.
    CommandList filled(Device device, QueueType type, Buffer target, uint64_t region)
    {
        CommandList list = device.acquireCommandList(type, "test.list");
        list.fillBuffer(target, region * kRegion, kRegion, kPattern);
        list.end();
        return list;
    }

    //==================================================================================================================
    /// Returns a buffer of `regions` regions that fills can write.
    Buffer fillTarget(Device device, uint64_t regions)
    {
        return *device.createBuffer({ .size = regions * kRegion, .usage = BufferUsage::ShaderWrite, .label = "test.target" });
    }

    //==================================================================================================================
    /// Submits `list`, a Compute list, signalling `fence` to `value`, and joins it into the frame's graphics queue.
    void submitCompute(Device device, CommandList list, Fence fence, uint64_t value)
    {
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, value } };
        device.queue(QueueType::Compute).submit(std::array{ list }, {}, signal);
        device.queue(QueueType::Graphics).submit({}, signal, {});
    }
} // namespace

//======================================================================================================================
TEST_CASE("a timed queue reports one span per submission that names lists, with serials from 1", "[d3d12]")
{
    Device device = *createDevice({ .framesInFlight = kFramesInFlight });
    const Buffer target = fillTarget(device, 5);
    Queue graphics = device.queue(QueueType::Graphics);
    Recorder recorder;
    d3d12::setSubmitTiming(graphics, record, &recorder);
    device.beginFrame();
    const std::array<CommandList, 4> lists{ filled(device, QueueType::Graphics, target, 0),
                                            filled(device, QueueType::Graphics, target, 1),
                                            filled(device, QueueType::Graphics, target, 2),
                                            filled(device, QueueType::Graphics, target, 3) };
    // Three submissions: of one list, of two, and of one.
    graphics.submit(std::span(lists).subspan(0, 1), {}, {});
    graphics.submit(std::span(lists).subspan(1, 2), {}, {});
    graphics.submit(std::span(lists).subspan(3, 1), {}, {});
    d3d12::drainSubmitTiming(graphics);
    REQUIRE(recorder.times.size() == 3);
    for (size_t i = 0; i < recorder.times.size(); ++i)
    {
        CAPTURE(i);
        CHECK(recorder.times[i].queue == QueueType::Graphics);
        CHECK(recorder.times[i].serial == i + 1);
        CHECK_FALSE(recorder.times[i].failed);
    }
    // A drain reports each submission once.
    d3d12::drainSubmitTiming(graphics);
    CHECK(recorder.times.size() == 3);
    device.endFrame();

    // Serials continue across drains and frames.
    device.beginFrame();
    graphics.submit(std::array{ filled(device, QueueType::Graphics, target, 4) }, {}, {});
    d3d12::drainSubmitTiming(graphics);
    REQUIRE(recorder.times.size() == 4);
    CHECK(recorder.times[3].serial == 4);
    CHECK_FALSE(recorder.times[3].failed);
    device.endFrame();
    d3d12::setSubmitTiming(graphics, nullptr, nullptr);
    device.destroy(target);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a submission without lists reports nothing", "[d3d12]")
{
    Device device = *createDevice({ .framesInFlight = kFramesInFlight });
    const Buffer target = fillTarget(device, 1);
    const Fence fence = *device.createFence(0, "test.fence");
    Queue graphics = device.queue(QueueType::Graphics);
    Recorder recorder;
    d3d12::setSubmitTiming(graphics, record, &recorder);
    device.beginFrame();
    const std::array<FenceValue, 1> value{ FenceValue{ fence, 1 } };
    graphics.submit({}, {}, value);
    graphics.submit({}, value, {});
    d3d12::drainSubmitTiming(graphics);
    CHECK(recorder.times.empty());
    // The fence-only submissions took no serial, so the first with a list takes 1.
    graphics.submit(std::array{ filled(device, QueueType::Graphics, target, 0) }, {}, {});
    d3d12::drainSubmitTiming(graphics);
    REQUIRE(recorder.times.size() == 1);
    CHECK(recorder.times[0].serial == 1);
    CHECK_FALSE(recorder.times[0].failed);
    device.endFrame();
    // Destroyed while attached: the device releases what the seam allocated.
    device.destroy(fence);
    device.destroy(target);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("submission spans start no later than they end and share the CPU time base across queues", "[d3d12]")
{
    Device device = *createDevice({ .framesInFlight = kFramesInFlight });
    const Buffer target = fillTarget(device, 2);
    const Fence fence = *device.createFence(0, "test.fence");
    Queue graphics = device.queue(QueueType::Graphics);
    Queue compute = device.queue(QueueType::Compute);
    Recorder graphicsTimes;
    Recorder computeTimes;
    d3d12::setSubmitTiming(graphics, record, &graphicsTimes);
    d3d12::setSubmitTiming(compute, record, &computeTimes);
    const double before = cpuSeconds();
    device.beginFrame();
    const CommandList first = filled(device, QueueType::Graphics, target, 0);
    const CommandList second = filled(device, QueueType::Compute, target, 1);
    // The compute list waits for the graphics list, then the graphics queue waits for it, which joins it into the frame.
    const std::array<FenceValue, 1> one{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> two{ FenceValue{ fence, 2 } };
    graphics.submit(std::array{ first }, {}, one);
    compute.submit(std::array{ second }, one, two);
    graphics.submit({}, two, {});
    d3d12::drainSubmitTiming(graphics);
    d3d12::drainSubmitTiming(compute);
    const double after = cpuSeconds();
    device.endFrame();

    REQUIRE(graphicsTimes.times.size() == 1);
    REQUIRE(computeTimes.times.size() == 1);
    const d3d12::SubmitTime& graphicsTime = graphicsTimes.times[0];
    const d3d12::SubmitTime& computeTime = computeTimes.times[0];
    CHECK(computeTime.queue == QueueType::Compute);
    for (const d3d12::SubmitTime& time : { graphicsTime, computeTime })
    {
        CAPTURE(time.queue, time.gpuStartSeconds, time.gpuEndSeconds, before, after);
        CHECK_FALSE(time.failed);
        CHECK(time.gpuStartSeconds <= time.gpuEndSeconds);
        // Both queues' times are QueryPerformanceCounter seconds: each span lies within the CPU's window around it.
        CHECK(time.gpuStartSeconds >= before - kWindowSlack);
        CHECK(time.gpuEndSeconds <= after + kWindowSlack);
    }
    CAPTURE(graphicsTime.gpuEndSeconds, computeTime.gpuStartSeconds);
    CHECK(computeTime.gpuStartSeconds >= graphicsTime.gpuEndSeconds - kQueueSlack);
    d3d12::setSubmitTiming(graphics, nullptr, nullptr);
    d3d12::setSubmitTiming(compute, nullptr, nullptr);
    device.destroy(fence);
    device.destroy(target);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the seam's timestamps count no native call", "[d3d12]")
{
    Device device = *createDevice({ .framesInFlight = kFramesInFlight });
    const Buffer target = fillTarget(device, kFramesInFlight + 2);
    Queue graphics = device.queue(QueueType::Graphics);
    uint64_t region = 0;
    // The counts of one list's acquisition, fill, end and submission.
    const auto submitOne = [&] {
        const Counters before = device.counters();
        graphics.submit(std::array{ filled(device, QueueType::Graphics, target, region++) }, {}, {});
        const Counters after = device.counters();
        return Counters{ .nativeCreateCalls = after.nativeCreateCalls - before.nativeCreateCalls,
                         .nativeRecordCalls = after.nativeRecordCalls - before.nativeRecordCalls,
                         .nativeSubmitCalls = after.nativeSubmitCalls - before.nativeSubmitCalls,
                         .nativeHostCalls = after.nativeHostCalls - before.nativeHostCalls,
                         .cpuAllocations = after.cpuAllocations - before.cpuAllocations,
                         .gpuAllocations = after.gpuAllocations - before.gpuAllocations };
    };
    // Once every frame slot's pool holds a list named "test.list", an acquisition creates and names nothing.
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
    {
        device.beginFrame();
        (void)submitOne();
        device.endFrame();
    }
    device.beginFrame();
    const Counters untimed = submitOne();
    device.endFrame();

    Recorder recorder;
    const Counters beforeAttach = device.counters();
    d3d12::setSubmitTiming(graphics, record, &recorder);
    CHECK(device.counters().nativeCreateCalls == beforeAttach.nativeCreateCalls);
    CHECK(device.counters().nativeHostCalls == beforeAttach.nativeHostCalls);
    CHECK(device.counters().gpuAllocations == beforeAttach.gpuAllocations);
    device.beginFrame();
    const Counters timed = submitOne();
    const Counters beforeDrain = device.counters();
    d3d12::drainSubmitTiming(graphics);
    CHECK(device.counters().nativeHostCalls == beforeDrain.nativeHostCalls);
    CHECK(device.counters().nativeSubmitCalls == beforeDrain.nativeSubmitCalls);
    device.endFrame();

    REQUIRE(recorder.times.size() == 1);
    CHECK_FALSE(recorder.times[0].failed);
    CHECK(timed.nativeCreateCalls == untimed.nativeCreateCalls);
    CHECK(timed.nativeRecordCalls == untimed.nativeRecordCalls);
    CHECK(timed.nativeSubmitCalls == untimed.nativeSubmitCalls);
    CHECK(timed.nativeHostCalls == untimed.nativeHostCalls);
    CHECK(timed.cpuAllocations == untimed.cpuAllocations);
    CHECK(timed.gpuAllocations == untimed.gpuAllocations);
    d3d12::setSubmitTiming(graphics, nullptr, nullptr);
    device.destroy(target);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a detached queue reports nothing and its lists write no timestamp", "[d3d12]")
{
    Device device = *createDevice({ .framesInFlight = kFramesInFlight });
    const Buffer target = fillTarget(device, 4);
    const Fence fence = *device.createFence(0, "test.fence");
    Queue graphics = device.queue(QueueType::Graphics);
    Recorder recorder;
    device.beginFrame();
    d3d12::setSubmitTiming(graphics, record, &recorder);
    const CommandList timed = filled(device, QueueType::Graphics, target, 0);
    CHECK(CommandListImpl::of(timed).state.submitTiming != nullptr);
    graphics.submit(std::array{ timed }, {}, {});
    // Detaching discards the submission not yet reported, and a list acquired afterwards claims no query.
    d3d12::setSubmitTiming(graphics, nullptr, nullptr);
    const CommandList untimed = filled(device, QueueType::Graphics, target, 1);
    CHECK(CommandListImpl::of(untimed).state.submitTiming == nullptr);
    CHECK(QueueImpl::of(graphics).timings.front()->claimed.load() == 1);
    graphics.submit(std::array{ untimed }, {}, {});
    d3d12::drainSubmitTiming(graphics);
    CHECK(recorder.times.empty());

    // A queue never attached reports nothing either, and its lists claim no query.
    const CommandList computeList = filled(device, QueueType::Compute, target, 2);
    CHECK(CommandListImpl::of(computeList).state.submitTiming == nullptr);
    submitCompute(device, computeList, fence, 1);
    d3d12::drainSubmitTiming(device.queue(QueueType::Compute));
    CHECK(recorder.times.empty());

    // A list acquired while the queue was detached and submitted once it is attached again wrote no timestamp: its
    // submission reports, failed.
    const CommandList early = filled(device, QueueType::Graphics, target, 3);
    d3d12::setSubmitTiming(graphics, record, &recorder);
    graphics.submit(std::array{ early }, {}, {});
    d3d12::drainSubmitTiming(graphics);
    REQUIRE(recorder.times.size() == 1);
    CHECK(recorder.times[0].serial == 1);
    CHECK(recorder.times[0].failed);
    device.endFrame();
    d3d12::setSubmitTiming(graphics, nullptr, nullptr);
    device.destroy(fence);
    device.destroy(target);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("drainSubmitTiming reports on the calling thread", "[d3d12]")
{
    Device device = *createDevice({ .framesInFlight = kFramesInFlight });
    const Buffer target = fillTarget(device, 2);
    Queue graphics = device.queue(QueueType::Graphics);
    Recorder recorder;
    d3d12::setSubmitTiming(graphics, record, &recorder);
    device.beginFrame();
    graphics.submit(std::array{ filled(device, QueueType::Graphics, target, 0) }, {}, {});
    graphics.submit(std::array{ filled(device, QueueType::Graphics, target, 1) }, {}, {});
    // The submissions are made on this thread and drained on another, which the join orders after them.
    std::thread::id drainer;
    std::thread thread([&] {
        drainer = std::this_thread::get_id();
        d3d12::drainSubmitTiming(graphics);
    });
    thread.join();
    device.endFrame();
    REQUIRE(recorder.threads.size() == 2);
    CHECK(drainer != std::this_thread::get_id());
    CHECK(recorder.threads[0] == drainer);
    CHECK(recorder.threads[1] == drainer);
    d3d12::setSubmitTiming(graphics, nullptr, nullptr);
    device.destroy(target);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a submission past the seam's ring or whose queries were claimed again reports failed, with contiguous serials", "[d3d12]")
{
    Device device = *createDevice({ .framesInFlight = kFramesInFlight });
    Queue graphics = device.queue(QueueType::Graphics);
    Recorder recorder;
    d3d12::setSubmitTiming(graphics, record, &recorder);

    // One submission more than the ring holds between two drains, of one empty list each, over several frames: the ring's
    // submissions report their spans and the one past it reports failed, with the next serial.
    constexpr uint32_t kRingFrames = 41;
    constexpr uint32_t kRingSubmissions = 25;
    static_assert(kRingFrames * kRingSubmissions == d3d12::SubmitTimingState::kPendingSubmissions + 1);
    for (uint32_t frame = 0; frame < kRingFrames; ++frame)
    {
        device.beginFrame();
        for (uint32_t submission = 0; submission < kRingSubmissions; ++submission)
        {
            CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
            list.end();
            graphics.submit(std::array{ list }, {}, {});
        }
        device.endFrame();
    }
    d3d12::drainSubmitTiming(graphics);
    REQUIRE(recorder.times.size() == d3d12::SubmitTimingState::kPendingSubmissions + 1);
    for (size_t i = 0; i < recorder.times.size(); ++i)
    {
        CAPTURE(i);
        CHECK(recorder.times[i].serial == i + 1);
        CHECK(recorder.times[i].failed == (i == d3d12::SubmitTimingState::kPendingSubmissions));
    }

    // More lists than the seam has query pairs, one submission of all of a frame's lists per frame, with no drain between:
    // only the first submission's pairs were claimed again before the drain read them, so only it reports failed. The
    // ring part already claimed 1025 pairs; the check does not depend on where this part starts, since a pair is claimed
    // again only kPairs claims after it.
    recorder.times.clear();
    constexpr uint32_t kPairFrames = 64;
    constexpr uint32_t kPairLists = 65;
    // The lists after the first frame's claim its pairs again; those after the second frame's do not reach its pairs.
    static_assert(kPairFrames * kPairLists > d3d12::SubmitTimingState::kPairs);
    static_assert(kPairFrames * kPairLists <= d3d12::SubmitTimingState::kPairs + kPairLists);
    std::array<CommandList, kPairLists> lists;
    for (uint32_t frame = 0; frame < kPairFrames; ++frame)
    {
        device.beginFrame();
        for (CommandList& list : lists)
        {
            list = device.acquireCommandList(QueueType::Graphics, "test.list");
            list.end();
        }
        graphics.submit(lists, {}, {});
        device.endFrame();
    }
    d3d12::drainSubmitTiming(graphics);
    REQUIRE(recorder.times.size() == kPairFrames);
    for (size_t i = 0; i < recorder.times.size(); ++i)
    {
        CAPTURE(i);
        CHECK(recorder.times[i].serial == d3d12::SubmitTimingState::kPendingSubmissions + 2 + i);
        CHECK(recorder.times[i].failed == (i == 0));
    }
    d3d12::setSubmitTiming(graphics, nullptr, nullptr);
    destroyDevice(device);
}
