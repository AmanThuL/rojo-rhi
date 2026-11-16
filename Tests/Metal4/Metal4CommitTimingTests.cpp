//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4CommitTimingTests.cpp
/// @brief Tests the Metal 4 backend's commit timing seam: one report per commit, per queue, from Metal's completion thread.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "CommitTiming.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The commits one queue reported, pushed from Metal's completion thread.
    struct Reports
    {
        std::mutex mutex;
        std::condition_variable arrived;
        std::vector<metal4::CommitTime> times;

        //==============================================================================================================
        /// Waits up to 10 seconds for `count` reports; feedback can arrive after the fence the work signals.
        bool waitFor(size_t count)
        {
            std::unique_lock lock(mutex);
            return arrived.wait_for(lock, std::chrono::seconds(10), [&] { return times.size() >= count; });
        }

        //==============================================================================================================
        /// Returns how many reports have arrived.
        size_t size()
        {
            const std::lock_guard lock(mutex);
            return times.size();
        }
    };

    //==================================================================================================================
    /// The callback every case attaches; `user` is a `Reports`.
    void collect(void* user, const metal4::CommitTime& time)
    {
        auto& reports = *static_cast<Reports*>(user);
        const std::lock_guard lock(reports.mutex);
        reports.times.push_back(time);
        reports.arrived.notify_all();
    }

    //==================================================================================================================
    /// Gives feedback that should not arrive time to do so. It only bounds a negative check: it cannot prove a report never comes.
    void settle()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    //==================================================================================================================
    /// Records one copy in a list of `type`, submits it alone to the queue of `type` and waits for the GPU on the host.
    void submitCopy(Device device, QueueType type, Buffer from, Buffer to)
    {
        CommandList list = device.acquireCommandList(type, "test.list");
        list.copyBuffer(from, 0, to, 0, 256);
        list.end();
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(type).submit(std::array{ list }, {}, signal);
        REQUIRE(device.wait({ fence, 1 }, UINT64_MAX));
    }
} // namespace

//======================================================================================================================
TEST_CASE("a queue with a sink reports one GPU span per commit, each starting before it ends", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    Reports graphics;
    metal4::setCommitTiming(device.queue(QueueType::Graphics), collect, &graphics);
    device.beginFrame();
    for (int commit = 0; commit < 3; ++commit)
    {
        submitCopy(device, QueueType::Graphics, a, b);
    }
    REQUIRE(graphics.waitFor(3));
    {
        const std::lock_guard lock(graphics.mutex);
        REQUIRE(graphics.times.size() == 3);
        for (size_t index = 0; index < 3; ++index)
        {
            const metal4::CommitTime& time = graphics.times[index];
            CHECK(time.queue == QueueType::Graphics);
            CHECK(time.serial == index + 1);
            CHECK_FALSE(time.failed);
            CHECK(time.gpuStartSeconds > 0.0);
            CHECK(time.gpuStartSeconds <= time.gpuEndSeconds);
        }
    }
    device.endFrame();
    metal4::setCommitTiming(device.queue(QueueType::Graphics), nullptr, nullptr);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a sink on one queue reports nothing of another queue's commits", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    Reports compute;
    metal4::setCommitTiming(device.queue(QueueType::Compute), collect, &compute);
    device.beginFrame();
    submitCopy(device, QueueType::Graphics, a, b);
    submitCopy(device, QueueType::Compute, a, b);
    REQUIRE(compute.waitFor(1));
    CHECK(compute.size() == 1);
    {
        const std::lock_guard lock(compute.mutex);
        CHECK(compute.times[0].queue == QueueType::Compute);
        CHECK(compute.times[0].serial == 1);
    }
    device.endFrame();
    metal4::setCommitTiming(device.queue(QueueType::Compute), nullptr, nullptr);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a queue whose sink was removed reports nothing more", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    Reports graphics;
    metal4::setCommitTiming(device.queue(QueueType::Graphics), collect, &graphics);
    device.beginFrame();
    submitCopy(device, QueueType::Graphics, a, b);
    REQUIRE(graphics.waitFor(1));
    metal4::setCommitTiming(device.queue(QueueType::Graphics), nullptr, nullptr);
    submitCopy(device, QueueType::Graphics, a, b);
    device.endFrame();
    // Destroying the device drains every queue, so any report owed by the second commit has arrived by then.
    destroyDevice(device);
    CHECK(graphics.size() == 1);
}

//======================================================================================================================
TEST_CASE("a sink set again counts its serials from one", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    Reports first;
    Reports second;
    metal4::setCommitTiming(device.queue(QueueType::Graphics), collect, &first);
    device.beginFrame();
    submitCopy(device, QueueType::Graphics, a, b);
    submitCopy(device, QueueType::Graphics, a, b);
    REQUIRE(first.waitFor(2));
    metal4::setCommitTiming(device.queue(QueueType::Graphics), collect, &second);
    submitCopy(device, QueueType::Graphics, a, b);
    REQUIRE(second.waitFor(1));
    {
        const std::lock_guard lock(second.mutex);
        CHECK(second.times[0].serial == 1);
    }
    device.endFrame();
    metal4::setCommitTiming(device.queue(QueueType::Graphics), nullptr, nullptr);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a submission with no lists commits nothing, so the next commit reports serial one and alone", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    Reports graphics;
    metal4::setCommitTiming(device.queue(QueueType::Graphics), collect, &graphics);
    device.beginFrame();
    const Fence fence = *device.createFence(0, "test.done");
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    device.queue(QueueType::Graphics).submit({}, {}, signal);
    REQUIRE(device.wait({ fence, 1 }, UINT64_MAX));
    submitCopy(device, QueueType::Graphics, a, b);
    REQUIRE(graphics.waitFor(1));
    settle();
    {
        const std::lock_guard lock(graphics.mutex);
        REQUIRE(graphics.times.size() == 1);
        CHECK(graphics.times[0].serial == 1);
    }
    device.endFrame();
    metal4::setCommitTiming(device.queue(QueueType::Graphics), nullptr, nullptr);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("detaching after a commit delivers that commit's report at most once and reports no later commit", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    Reports graphics;
    metal4::setCommitTiming(device.queue(QueueType::Graphics), collect, &graphics);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.copyBuffer(a, 0, b, 0, 256);
    list.end();
    const Fence fence = *device.createFence(0, "test.done");
    device.queue(QueueType::Graphics).submit(std::array{ list }, {}, std::array{ FenceValue{ fence, 1 } });
    // The report of the commit made before the detach may arrive after it, and arrives exactly once.
    metal4::setCommitTiming(device.queue(QueueType::Graphics), nullptr, nullptr);
    REQUIRE(device.wait({ fence, 1 }, 10'000'000'000));
    REQUIRE(graphics.waitFor(1));
    submitCopy(device, QueueType::Graphics, a, b);
    settle();
    {
        const std::lock_guard lock(graphics.mutex);
        REQUIRE(graphics.times.size() == 1);
        CHECK(graphics.times[0].serial == 1);
    }
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a sink leaves the device counters as they are for the same work", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    const auto grown = [&] {
        const Counters before = device.counters();
        submitCopy(device, QueueType::Graphics, a, b);
        const Counters after = device.counters();
        return std::array<uint64_t, 4>{ after.nativeRecordCalls - before.nativeRecordCalls,
                                        after.nativeSubmitCalls - before.nativeSubmitCalls,
                                        after.nativeHostCalls - before.nativeHostCalls,
                                        after.nativeCreateCalls - before.nativeCreateCalls };
    };
    submitCopy(device, QueueType::Graphics, a, b);
    const std::array<uint64_t, 4> without = grown();
    Reports graphics;
    metal4::setCommitTiming(device.queue(QueueType::Graphics), collect, &graphics);
    const std::array<uint64_t, 4> with = grown();
    CHECK(with == without);
    REQUIRE(graphics.waitFor(1));
    device.endFrame();
    metal4::setCommitTiming(device.queue(QueueType::Graphics), nullptr, nullptr);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a sink on a caller queue leaves stager uploads untimed", "[metal4]")
{
    Device device = *createDevice();
    Reports graphics;
    metal4::setCommitTiming(device.queue(QueueType::Graphics), collect, &graphics);
    const std::vector<std::byte> bytes(256, std::byte{ 7 });
    const Buffer staged = *device.createBuffer({ .size = 256, .label = "test.staged" }, bytes);
    const Buffer other = *device.createBuffer({ .size = 256, .label = "test.other" });
    device.beginFrame();
    settle();
    CHECK(graphics.size() == 0);
    submitCopy(device, QueueType::Graphics, staged, other);
    REQUIRE(graphics.waitFor(1));
    CHECK(graphics.times[0].serial == 1);
    device.endFrame();
    metal4::setCommitTiming(device.queue(QueueType::Graphics), nullptr, nullptr);
    destroyDevice(device);
}
