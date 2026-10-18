//----------------------------------------------------------------------------------------------------------------------
/// @file QueueConformanceTests.cpp
/// @brief Observes fences and submission order of chapter 10 on a GPU.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("a value signalled by a submission with no lists completes on the GPU and Device::wait sees it", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice();
    const Fence fence = *device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 3 } };
    device.queue(QueueType::Graphics).submit({}, {}, signal);
    CHECK(device.wait({ fence, 3 }, UINT64_MAX));
    CHECK(device.completedValue(fence) == 3);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two queues waiting on each other's fence values both reach their last values on the GPU", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice();
    const Fence f = *device.createFence(0, "test.f");
    const Fence g = *device.createFence(0, "test.g");
    // A wait names only values already submitted for signal (VU-rojo-queue-0007), so the first value goes first and the
    // queues then wait on each other in turn: g reaches 2 only through f at 2, which waits for g at 1.
    const auto one = [](Fence fence, uint64_t value) { return std::array<FenceValue, 1>{ FenceValue{ fence, value } }; };
    Queue graphics = device.queue(QueueType::Graphics);
    Queue compute = device.queue(QueueType::Compute);
    graphics.submit({}, {}, one(f, 1));
    compute.submit({}, one(f, 1), one(g, 1));
    graphics.submit({}, one(g, 1), one(f, 2));
    compute.submit({}, one(f, 2), one(g, 2));
    CHECK(device.wait({ g, 2 }, UINT64_MAX));
    CHECK(device.completedValue(f) == 2);
    CHECK(device.completedValue(g) == 2);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a chain of waits and signals through the three queues reaches its last value on the GPU", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice();
    const Fence fence = *device.createFence(0, "test.chain");
    const std::array<QueueType, 3> queues{ QueueType::Copy, QueueType::Compute, QueueType::Graphics };
    // Each step waits for the value the step before it signals, on a different queue from that step's.
    for (uint64_t step = 1; step <= 9; ++step)
    {
        const std::array<FenceValue, 1> wait{ FenceValue{ fence, step - 1 } };
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, step } };
        device.queue(queues[step % queues.size()]).submit({}, wait, signal);
    }
    CHECK(device.wait({ fence, 9 }, UINT64_MAX));
    CHECK(device.completedValue(fence) == 9);
    destroyDevice(device);
}
