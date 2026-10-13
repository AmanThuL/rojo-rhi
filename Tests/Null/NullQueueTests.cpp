//----------------------------------------------------------------------------------------------------------------------
/// @file NullQueueTests.cpp
/// @brief Tests what only the Null backend promises about queues and fences: work completes during the submit call.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("a Null submission completes its signals before submit returns", "[null]")
{
    Device device = *createDevice();
    const Fence fence = *device.createFence(0, "null.fence");
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 100 } };
    device.queue(QueueType::Copy).submit({}, {}, signal);
    CHECK(device.completedValue(fence) == 100);
    CHECK(device.wait({ fence, 100 }, 0));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null wait on a value below the signalled one returns at once", "[null]")
{
    Device device = *createDevice();
    const Fence fence = *device.createFence(0, "null.fence");
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 100 } };
    device.queue(QueueType::Graphics).submit({}, {}, signal);
    CHECK(device.wait({ fence, 50 }, UINT64_MAX));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null wait that cannot be satisfied returns false without blocking", "[null]")
{
    Device device = *createDevice();
    const Fence fence = *device.createFence(0, "null.fence");
    CHECK_FALSE(device.wait({ fence, 1 }, 10'000'000'000));
    destroyDevice(device);
}
