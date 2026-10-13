//----------------------------------------------------------------------------------------------------------------------
/// @file QueueDeathTests.cpp
/// @brief Death tests for the valid-usage rules of queues and fences in chapter 10.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0003 a fence appears twice in one signal list", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0003", [] {
        Device device = *createDevice();
        const Fence fence = *device.createFence(0, "test.fence");
        const std::array<FenceValue, 2> signal{ FenceValue{ fence, 1 }, FenceValue{ fence, 2 } };
        device.queue(QueueType::Graphics).submit({}, {}, signal);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0003 a signal value is not above the initial value", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0003", [] {
        Device device = *createDevice();
        const Fence fence = *device.createFence(5, "test.fence");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 5 } };
        device.queue(QueueType::Graphics).submit({}, {}, signal);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0003 a signal value is not above the last submitted value", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0003", [] {
        Device device = *createDevice();
        const Fence fence = *device.createFence(0, "test.fence");
        Queue queue = device.queue(QueueType::Graphics);
        const std::array<FenceValue, 1> first{ FenceValue{ fence, 4 } };
        queue.submit({}, {}, first);
        const std::array<FenceValue, 1> second{ FenceValue{ fence, 3 } };
        queue.submit({}, {}, second);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0007 a wait value was above the greatest submitted for signal", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0007", [] {
        Device device = *createDevice();
        const Fence fence = *device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> wait{ FenceValue{ fence, 1 } };
        device.queue(QueueType::Compute).submit({}, wait);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0008 an unlimited wait names a value above the greatest submitted for signal", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0008", [] {
        Device device = *createDevice();
        const Fence fence = *device.createFence(0, "test.fence");
        (void)device.wait({ fence, 1 }, UINT64_MAX);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0004 a default-constructed Queue submits", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0004", [] {
        Queue queue;
        queue.submit({});
    });
}
