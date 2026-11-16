//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ScopeDurationTests.cpp
/// @brief Tests through the public entries the durations, order and queues of D3D12 scope timings around fills and of empty scopes.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

using namespace rojoRHI;

namespace {
    /// The frames in flight of every case's device.
    constexpr uint32_t kFramesInFlight = 2;

    /// The size of the fill each timed scope encloses: long enough to take measurable GPU time.
    constexpr uint64_t kBig = uint64_t{ 64 } << 20;

    /// A fill pattern whose bytes differ.
    constexpr uint32_t kUnequal = 0x01020304u;

    //==================================================================================================================
    /// Ends `lists`, submits each to the queue of its type, joins the compute lists into the frame's graphics queue through
    /// a fence, waits on the host for all of them and destroys the fence.
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
        device.destroy(fence);
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

    //==================================================================================================================
    /// Returns true when `timing` is finite and at least zero.
    bool finiteAndNonNegative(const ScopeTiming& timing)
    {
        return std::isfinite(timing.gpuMilliseconds) && timing.gpuMilliseconds >= 0.0;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a scope around fills reports a positive duration", "[d3d12]")
{
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
    device.destroy(target);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("nested scopes around fills report parents before children with finite non-negative durations", "[d3d12]")
{
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
    // Chapter 14, `ScopeTiming`: nested scopes overlap their parent, so on one list a parent's timestamps enclose its
    // children's: no child outlasts it.
    CHECK(report[0].gpuMilliseconds > 0.0);
    CHECK(report[0].gpuMilliseconds >= report[1].gpuMilliseconds);
    CHECK(report[0].gpuMilliseconds >= report[2].gpuMilliseconds);
    CHECK(report[2].gpuMilliseconds >= report[3].gpuMilliseconds);
    device.endFrame();
    device.destroy(target);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("scopes around fills on the Compute queue are timed and reported with their queue", "[d3d12]")
{
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
    device.destroy(target);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an empty scope reports a finite duration of at least zero on D3D12", "[d3d12]")
{
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
