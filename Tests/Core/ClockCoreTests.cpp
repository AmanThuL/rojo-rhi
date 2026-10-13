//----------------------------------------------------------------------------------------------------------------------
/// @file ClockCoreTests.cpp
/// @brief Tests the vector clocks that order queues in the validation tracker, and the clocks it keeps per queue and fence.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Tracker/Clocks.h"
#include "Core/Tracker/Tracker.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

using namespace rojoRHI;
using test::PassKit;

namespace {
    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`, waiting on `wait` and signalling `signal`.
    [[maybe_unused]] void submitTo(Device device, CommandList list, QueueType type, std::span<const FenceValue> wait, std::span<const FenceValue> signal)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, wait, signal);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a clock join takes the greater position of each queue and a meet the lesser", "[core]")
{
    const core::Clock a{ 4, 0, 7 };
    const core::Clock b{ 2, 5, 7 };
    CHECK(core::join(a, b) == core::Clock{ 4, 5, 7 });
    CHECK(core::meet(a, b) == core::Clock{ 2, 0, 7 });
}

//======================================================================================================================
TEST_CASE("a clock covers the positions of a queue up to its component of that queue", "[core]")
{
    const core::Clock clock{ 4, 0, 2 };
    CHECK(core::covers(clock, QueueType::Graphics, 4));
    CHECK_FALSE(core::covers(clock, QueueType::Graphics, 5));
    CHECK_FALSE(core::covers(clock, QueueType::Compute, 1));
    CHECK(core::covers(clock, QueueType::Copy, 1));
}

//======================================================================================================================
TEST_CASE("a wait on a value at most a fence's initial value orders nothing", "[core]")
{
    core::FenceClocks fence(5);
    fence.signal(6, core::Clock{ 3, 0, 0 });
    CHECK(fence.reached(0) == std::nullopt);
    CHECK(fence.reached(5) == std::nullopt);
    CHECK(fence.reached(6) == core::Clock{ 3, 0, 0 });
}

//======================================================================================================================
TEST_CASE("a wait takes the meet of the clocks of every value at least it", "[core]")
{
    // Two queues signal one fence with no order between them: the fence may reach 2 before 1.
    core::FenceClocks fence(0);
    fence.signal(1, core::Clock{ 4, 0, 0 });
    fence.signal(2, core::Clock{ 0, 3, 0 });
    fence.signal(3, core::Clock{ 0, 5, 0 });
    CHECK(fence.reached(1) == core::Clock{ 0, 0, 0 });
    CHECK(fence.reached(2) == core::Clock{ 0, 3, 0 });
    CHECK(fence.reached(3) == core::Clock{ 0, 5, 0 });
    CHECK(fence.reached(4) == std::nullopt);
}

//======================================================================================================================
TEST_CASE("signals that each follow the one before keep each value's own clock", "[core]")
{
    core::FenceClocks fence(0);
    fence.signal(1, core::Clock{ 4, 0, 0 });
    fence.signal(2, core::Clock{ 4, 3, 0 });
    fence.signal(4, core::Clock{ 6, 3, 1 });
    CHECK(fence.reached(1) == core::Clock{ 4, 0, 0 });
    CHECK(fence.reached(2) == core::Clock{ 4, 3, 0 });
    CHECK(fence.reached(3) == core::Clock{ 6, 3, 1 });
}

//======================================================================================================================
TEST_CASE("a queue's running clock joins the clock of the value it waits on, and a CPU wait raises the host clock", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    core::Tracker& tracker = core::coreOf(kit.device).tracker();
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> done{ FenceValue{ fence, 1 } };
    kit.list.fillBuffer(buffer, 0, 256, 0);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, done);
    const uint64_t graphics = tracker.runningClock(QueueType::Graphics)[0];
    CHECK(graphics != 0);
    CHECK(tracker.hostClock() == core::Clock{});
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    submitTo(kit.device, compute, QueueType::Compute, done, {});
    CHECK(tracker.runningClock(QueueType::Compute)[0] == graphics);
    CHECK(tracker.runningClock(QueueType::Copy) == core::Clock{});
    CHECK(kit.device.wait(done[0], UINT64_MAX));
    CHECK(tracker.hostClock()[0] == graphics);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("values the host clock covers collapse into a floor that orders what the last of them ordered", "[core]")
{
    core::FenceClocks fence(0);
    fence.signal(1, core::Clock{ 1, 0, 0 });
    fence.signal(2, core::Clock{ 2, 0, 0 });
    fence.signal(3, core::Clock{ 3, 0, 0 });
    fence.signal(4, core::Clock{ 4, 0, 0 });
    fence.prune(core::Clock{ 2, 5, 0 });
    CHECK(fence.size() == 3);
    // Values at most the floor's take its clock: the host clock already covers it, so it orders nothing more.
    CHECK(fence.reached(1) == core::Clock{ 2, 0, 0 });
    CHECK(fence.reached(2) == core::Clock{ 2, 0, 0 });
    CHECK(fence.reached(3) == core::Clock{ 3, 0, 0 });
    CHECK(fence.reached(0) == std::nullopt);
    fence.prune(core::Clock{ 9, 9, 9 });
    CHECK(fence.size() == 1);
    CHECK(fence.reached(1) == core::Clock{ 4, 0, 0 });
}

//======================================================================================================================
TEST_CASE("pruning keeps a value whose stored meet the host clock does not cover", "[core]")
{
    // The meet of value 1 takes in value 2's clock, so it rises along the list and pruning stops at the first uncovered one.
    core::FenceClocks fence(0);
    fence.signal(1, core::Clock{ 4, 0, 0 });
    fence.signal(2, core::Clock{ 0, 3, 0 });
    fence.signal(3, core::Clock{ 5, 3, 0 });
    fence.prune(core::Clock{ 0, 2, 0 });
    CHECK(fence.size() == 3);
    fence.prune(core::Clock{ 0, 3, 0 });
    CHECK(fence.size() == 2);
    CHECK(fence.reached(1) == core::Clock{ 0, 3, 0 });
    CHECK(fence.reached(3) == core::Clock{ 5, 3, 0 });
}

//======================================================================================================================
TEST_CASE("a fence signalled every frame keeps about framesInFlight values", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    core::Tracker& tracker = core::coreOf(kit.device).tracker();
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    CommandList list = kit.list;
    for (uint64_t frame = 1; frame <= 64; ++frame)
    {
        if (frame != 1)
        {
            kit.device.beginFrame();
            list = kit.device.acquireCommandList(QueueType::Graphics, "test.frame");
        }
        const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite } };
        list.barrier(globals);
        list.fillBuffer(buffer, 0, 256, static_cast<uint32_t>(frame));
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, frame } };
        submitTo(kit.device, list, QueueType::Graphics, {}, signal);
        kit.device.endFrame();
    }
    // The device keeps three frames in flight: retiring a slot covers its frame's value, and pruning keeps it as the floor.
    const std::optional<size_t> kept = tracker.fenceValueCount(fence);
    REQUIRE(kept.has_value());
    CHECK(*kept <= 4);
    destroyDevice(kit.device);
#endif
}
