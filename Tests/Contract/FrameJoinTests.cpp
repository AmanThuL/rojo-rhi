//----------------------------------------------------------------------------------------------------------------------
/// @file FrameJoinTests.cpp
/// @brief Tests that the validation tracker accepts frames whose compute and copy work a graphics submission of the frame waits on.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// The bytes every test writes.
    [[maybe_unused]] constexpr uint64_t kBytes = 256;

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`, waiting on `wait` and signalling `signal`.
    [[maybe_unused]] void submitTo(
        Device device, CommandList list, QueueType type, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {}
    )
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, wait, signal);
    }

    //==================================================================================================================
    /// Acquires a compute list that fills a fresh buffer.
    [[maybe_unused]] CommandList computeWork(PassKit& kit)
    {
        const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite, kBytes);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.fillBuffer(buffer, 0, kBytes, 0);
        return compute;
    }

    //==================================================================================================================
    /// Acquires a copy list that copies between two fresh buffers.
    [[maybe_unused]] CommandList copyWork(PassKit& kit)
    {
        const Buffer source = kit.buffer(BufferUsage::None, kBytes);
        const Buffer destination = kit.buffer(BufferUsage::None, kBytes);
        CommandList copy = kit.device.acquireCommandList(QueueType::Copy, "test.copy");
        copy.copyBuffer(source, 0, destination, 0, kBytes);
        return copy;
    }
} // namespace

//======================================================================================================================
TEST_CASE("compute work of a frame is joined into its graphics work by a fence wait", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> done{ FenceValue{ fence, 1 } };
    submitTo(kit.device, computeWork(kit), QueueType::Compute, {}, done);
    submitTo(kit.device, kit.list, QueueType::Graphics, done);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("an earlier graphics submission's wait joins compute work into the frame", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> done{ FenceValue{ fence, 1 } };
    submitTo(kit.device, computeWork(kit), QueueType::Compute, {}, done);
    submitTo(kit.device, kit.list, QueueType::Graphics, done);
    // The frame's last graphics submission waits on nothing; the earlier one already joined the compute work.
    submitTo(kit.device, kit.device.acquireCommandList(QueueType::Graphics, "test.last"), QueueType::Graphics);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("the graphics submissions of a frame together join its compute and copy work", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Fence computed = *kit.device.createFence(0, "test.computed");
    const Fence copied = *kit.device.createFence(0, "test.copied");
    const std::array<FenceValue, 1> computeDone{ FenceValue{ computed, 1 } };
    const std::array<FenceValue, 1> copyDone{ FenceValue{ copied, 1 } };
    submitTo(kit.device, computeWork(kit), QueueType::Compute, {}, computeDone);
    submitTo(kit.device, copyWork(kit), QueueType::Copy, {}, copyDone);
    submitTo(kit.device, kit.list, QueueType::Graphics, computeDone);
    submitTo(kit.device, kit.device.acquireCommandList(QueueType::Graphics, "test.last"), QueueType::Graphics, copyDone);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("compute work is joined into the frame through a wait of the copy queue", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> computeDone{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> copyDone{ FenceValue{ fence, 2 } };
    submitTo(kit.device, computeWork(kit), QueueType::Compute, {}, computeDone);
    submitTo(kit.device, copyWork(kit), QueueType::Copy, computeDone, copyDone);
    submitTo(kit.device, kit.list, QueueType::Graphics, copyDone);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a submission with no lists to the compute queue is joined into the frame like any other", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> done{ FenceValue{ fence, 1 } };
    kit.device.queue(QueueType::Compute).submit({}, {}, done);
    submitTo(kit.device, kit.list, QueueType::Graphics, done);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a frame with no compute or copy work ends, after a frame whose work was joined", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> done{ FenceValue{ fence, 1 } };
    submitTo(kit.device, computeWork(kit), QueueType::Compute, {}, done);
    submitTo(kit.device, kit.list, QueueType::Graphics, done);
    kit.device.endFrame();
    kit.device.beginFrame();
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite, kBytes);
    CommandList graphics = kit.device.acquireCommandList(QueueType::Graphics, "test.graphics");
    graphics.fillBuffer(buffer, 0, kBytes, 0);
    submitTo(kit.device, graphics, QueueType::Graphics);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a submission with no lists made between frames belongs to no frame", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    submitTo(kit.device, kit.list, QueueType::Graphics);
    kit.device.endFrame();
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> done{ FenceValue{ fence, 1 } };
    kit.device.queue(QueueType::Compute).submit({}, {}, done);
    kit.device.beginFrame();
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}
