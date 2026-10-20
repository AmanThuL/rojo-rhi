//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4CounterTests.cpp
/// @brief Pins the native calls the Metal 4 backend counts for frames, submissions, residency, staging and waits.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("a paced frame with fence submissions counts the Metal 4 native calls", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    Queue queue = device.queue(QueueType::Graphics);
    // Fill the frame slots, then commit the residency set the tables and creations dirtied.
    for (int frame = 0; frame < 2; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    queue.submit({});
    const Fence a = *device.createFence(0, "test.a");
    const Fence b = *device.createFence(0, "test.b");
    const std::array<FenceValue, 1> prime{ FenceValue{ a, 1 } };
    queue.submit({}, {}, prime);

    const Counters s0 = device.counters();
    device.beginFrame();
    const Counters s1 = device.counters();
    CHECK(s1.nativeHostCalls == s0.nativeHostCalls + 1);
    CHECK(s1.nativeSubmitCalls == s0.nativeSubmitCalls);

    device.endFrame();
    const Counters s2 = device.counters();
    CHECK(s2.nativeSubmitCalls == s1.nativeSubmitCalls + 1);
    CHECK(s2.nativeHostCalls == s1.nativeHostCalls);

    const std::array<FenceValue, 1> wait{ FenceValue{ a, 1 } };
    const std::array<FenceValue, 1> signal{ FenceValue{ b, 1 } };
    queue.submit({}, wait, signal);
    const Counters s3 = device.counters();
    // The wait and the signal: the graphics queue signals no progress, since a frame's retirement covers its submissions.
    CHECK(s3.nativeSubmitCalls == s2.nativeSubmitCalls + 2);
    CHECK(s3.nativeHostCalls == s2.nativeHostCalls);

    // A creation dirties the residency set, and the next submission commits it once.
    const Buffer buffer = *device.createBuffer({ .size = 64 });
    const Counters s4 = device.counters();
    queue.submit({});
    const Counters s5 = device.counters();
    CHECK(s5.nativeHostCalls == s4.nativeHostCalls + 1);

    // A staged Gpu buffer: one staging buffer beyond the pool's own count, one submission, and three residency commits: the
    // device set the new buffer dirtied, then the stager's set as the staging buffer joins and leaves it.
    const std::array<std::byte, 16> bytes{};
    const Counters plainStart = device.counters();
    (void)*device.createBuffer({ .size = 64 });
    const Counters plain = device.counters();
    (void)*device.createBuffer({ .size = 64 }, bytes);
    const Counters staged = device.counters();
    CHECK(staged.nativeSubmitCalls == plain.nativeSubmitCalls + 1);
    CHECK(staged.nativeCreateCalls - plain.nativeCreateCalls == plain.nativeCreateCalls - plainStart.nativeCreateCalls + 1);
    CHECK(staged.nativeHostCalls == plain.nativeHostCalls + 3);

    // A native wait counts one host call.
    CHECK(device.wait({ b, 1 }, UINT64_MAX));
    const Counters s6 = device.counters();
    CHECK(s6.nativeHostCalls == staged.nativeHostCalls + 1);
    device.destroy(buffer);
    destroyDevice(device);
}
