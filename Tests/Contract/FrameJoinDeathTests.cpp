//----------------------------------------------------------------------------------------------------------------------
/// @file FrameJoinDeathTests.cpp
/// @brief Death tests for compute and copy work of a frame that no graphics submission of the frame waits on.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// The bytes every test copies.
    constexpr uint64_t kBytes = 256;

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`.
    void submitTo(Device device, CommandList list, QueueType type)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-device-0006 compute work submitted in the frame is not joined into its graphics work", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0006", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite, kBytes);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        compute.fillBuffer(buffer, 0, kBytes, 0);
        submitTo(kit.device, compute, QueueType::Compute);
        submitTo(kit.device, kit.list, QueueType::Graphics);
        kit.device.endFrame();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0006 copy work submitted in the frame is not joined into its graphics work", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0006", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer source = kit.buffer(BufferUsage::None, kBytes);
        const Buffer destination = kit.buffer(BufferUsage::None, kBytes);
        CommandList copy = kit.device.acquireCommandList(QueueType::Copy, "test.copy");
        copy.copyBuffer(source, 0, destination, 0, kBytes);
        submitTo(kit.device, copy, QueueType::Copy);
        submitTo(kit.device, kit.list, QueueType::Graphics);
        kit.device.endFrame();
    });
}
