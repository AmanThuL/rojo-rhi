//----------------------------------------------------------------------------------------------------------------------
/// @file QueueTests.cpp
/// @brief Tests queues, fences, submission and waiting against chapter 10.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    Device makeDevice()
    {
        Result<Device> created = createDevice();
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    Fence makeFence(Device device, uint64_t initial)
    {
        Result<Fence> created = device.createFence(initial, "test.fence");
        REQUIRE(created.has_value());
        return *created;
    }
} // namespace

//======================================================================================================================
TEST_CASE("Device::queue returns the same queue for a type on every call", "[contract]")
{
    Device device = makeDevice();
    const Queue first = device.queue(QueueType::Compute);
    const Queue second = device.queue(QueueType::Compute);
    // Handles are opaque wrappers; the same queue must accept the same submission through either copy.
    const Fence fence = makeFence(device, 0);
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    Queue copy = first;
    copy.submit({}, {}, signal);
    CHECK(device.completedValue(fence) == 1);
    Queue other = second;
    const std::array<FenceValue, 1> next{ FenceValue{ fence, 2 } };
    other.submit({}, {}, next);
    CHECK(device.completedValue(fence) == 2);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a new fence reports its initial value", "[contract]")
{
    Device device = makeDevice();
    CHECK(device.completedValue(makeFence(device, 0)) == 0);
    CHECK(device.completedValue(makeFence(device, 41)) == 41);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("createFence accepts an empty label", "[contract]")
{
    Device device = makeDevice();
    CHECK(device.createFence(0, "").has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a submission with no lists signals its fences", "[contract]")
{
    Device device = makeDevice();
    const Fence a = makeFence(device, 0);
    const Fence b = makeFence(device, 5);
    const std::array<FenceValue, 2> signal{ FenceValue{ a, 3 }, FenceValue{ b, 9 } };
    Queue queue = device.queue(QueueType::Graphics);
    queue.submit({}, {}, signal);
    CHECK(device.completedValue(a) == 3);
    CHECK(device.completedValue(b) == 9);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a submission may wait for a value at most the initial value", "[contract]")
{
    Device device = makeDevice();
    const Fence fence = makeFence(device, 4);
    const std::array<FenceValue, 1> wait{ FenceValue{ fence, 4 } };
    Queue queue = device.queue(QueueType::Copy);
    queue.submit({}, wait);
    CHECK(device.completedValue(fence) == 4);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a submission may wait for a value an earlier submission signals", "[contract]")
{
    Device device = makeDevice();
    const Fence fence = makeFence(device, 0);
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 7 } };
    const std::array<FenceValue, 1> wait{ FenceValue{ fence, 7 } };
    device.queue(QueueType::Compute).submit({}, {}, signal);
    device.queue(QueueType::Graphics).submit({}, wait);
    CHECK(device.completedValue(fence) == 7);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a fence value may be signalled again with a greater value", "[contract]")
{
    Device device = makeDevice();
    const Fence fence = makeFence(device, 0);
    Queue queue = device.queue(QueueType::Graphics);
    for (uint64_t value = 1; value <= 3; ++value)
    {
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, value } };
        queue.submit({}, {}, signal);
        CHECK(device.completedValue(fence) == value);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Device::wait polls with a zero timeout", "[contract]")
{
    Device device = makeDevice();
    const Fence fence = makeFence(device, 2);
    CHECK(device.wait({ fence, 2 }, 0));
    CHECK_FALSE(device.wait({ fence, 3 }, 0));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Device::wait returns true for a value that was signalled", "[contract]")
{
    Device device = makeDevice();
    const Fence fence = makeFence(device, 0);
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 6 } };
    device.queue(QueueType::Graphics).submit({}, {}, signal);
    CHECK(device.wait({ fence, 6 }, UINT64_MAX));
    CHECK(device.wait({ fence, 1 }, UINT64_MAX));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Device::wait times out on a value that is never signalled", "[contract]")
{
    Device device = makeDevice();
    const Fence fence = makeFence(device, 0);
    CHECK_FALSE(device.wait({ fence, 10 }, 1'000));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a destroyed fence's handle is released like any resource", "[contract]")
{
    Device device = makeDevice();
    const Fence fence = makeFence(device, 0);
    device.destroy(fence);
    device.beginFrame();
    device.endFrame();
    destroyDevice(device);
}
