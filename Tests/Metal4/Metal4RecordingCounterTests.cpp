//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4RecordingCounterTests.cpp
/// @brief Pins the native calls per recorded operation of the Metal 4 backend, measured as the overhead note measures them.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The bytes every operation moves or writes.
    constexpr uint64_t kBytes = 256;

    /// The repetition counts of the pairs: the low and high count of the in-list measures, and of the per-submission one.
    constexpr uint32_t kLow = 100;
    constexpr uint32_t kHigh = 1000;
    constexpr uint32_t kSubmitLow = 10;

    /// What the operations of one case record into: two buffers of `kHigh` slices of `kBytes` bytes.
    struct Fixture
    {
        Device device = *createDevice();
        Buffer source;
        Buffer target;

        //==============================================================================================================
        Fixture()
        {
            source = *device.createBuffer({ .size = kBytes * kHigh, .usage = BufferUsage::ShaderWrite, .label = "test.source" });
            target = *device.createBuffer({ .size = kBytes * kHigh, .usage = BufferUsage::ShaderWrite, .label = "test.target" });
        }

        //==============================================================================================================
        ~Fixture()
        {
            destroyDevice(device);
        }

        //==============================================================================================================
        Fixture(const Fixture&) = delete;

        //==============================================================================================================
        Fixture& operator=(const Fixture&) = delete;

        //==============================================================================================================
        /// Runs a frame in which one list of `type` records `count` repetitions of `record`, then submits it and waits for it.
        /// Returns the counters' growth from before the list is acquired to after the wait.
        Counters measure(QueueType type, uint32_t count, const std::function<void(CommandList, uint32_t)>& record)
        {
            device.beginFrame();
            const Counters before = device.counters();
            CommandList list = device.acquireCommandList(type, "test.list");
            for (uint32_t index = 0; index < count; ++index)
            {
                record(list, index);
            }
            list.end();
            const Fence fence = *device.createFence(0, "test.done");
            const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
            device.queue(type).submit(std::array{ list }, {}, signal);
            REQUIRE(device.wait({ fence, 1 }, UINT64_MAX));
            const Counters after = device.counters();
            device.endFrame();
            return Counters{ .nativeRecordCalls = after.nativeRecordCalls - before.nativeRecordCalls,
                             .nativeSubmitCalls = after.nativeSubmitCalls - before.nativeSubmitCalls,
                             .nativeBarriers = after.nativeBarriers - before.nativeBarriers };
        }
    };

    //==================================================================================================================
    /// The exact per-repetition growth of `field` between a low and a high count; fails the case when it is not a whole number.
    uint64_t perOperation(const Counters& low, const Counters& high, uint64_t Counters::* field)
    {
        const uint64_t grown = high.*field - low.*field;
        REQUIRE(grown % (kHigh - kLow) == 0);
        return grown / (kHigh - kLow);
    }

    //==================================================================================================================
    /// A global barrier from copy writes to copy reads and writes.
    GlobalBarrier copyToCopy()
    {
        return GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead | Access::CopyWrite
        };
    }
} // namespace

//======================================================================================================================
TEST_CASE("uploadFrameData of 256 bytes makes no native call", "[metal4]")
{
    Fixture fixture;
    const std::vector<std::byte> bytes(kBytes);
    const auto record = [&](CommandList list, uint32_t) { list.uploadFrameData(bytes); };
    const Counters low = fixture.measure(QueueType::Graphics, kLow, record);
    const Counters high = fixture.measure(QueueType::Graphics, kHigh, record);
    CHECK(perOperation(low, high, &Counters::nativeRecordCalls) == 0);
    CHECK(perOperation(low, high, &Counters::nativeSubmitCalls) == 0);
}

//======================================================================================================================
TEST_CASE("copyBuffer of 256 bytes is one native call", "[metal4]")
{
    Fixture fixture;
    const auto record = [&](CommandList list, uint32_t index) { list.copyBuffer(fixture.source, index * kBytes, fixture.target, index * kBytes, kBytes); };
    const Counters low = fixture.measure(QueueType::Graphics, kLow, record);
    const Counters high = fixture.measure(QueueType::Graphics, kHigh, record);
    CHECK(perOperation(low, high, &Counters::nativeRecordCalls) == 1);
}

//======================================================================================================================
TEST_CASE("an equal-byte fill is one native call and an unequal one five once the encoder holds the kernel pipeline", "[metal4]")
{
    Fixture fixture;
    const auto equal = [&](CommandList list, uint32_t index) { list.fillBuffer(fixture.target, index * kBytes, kBytes, 0x5A5A5A5Au); };
    const auto unequal = [&](CommandList list, uint32_t index) { list.fillBuffer(fixture.target, index * kBytes, kBytes, 0x01020304u); };
    CHECK(
        perOperation(fixture.measure(QueueType::Graphics, kLow, equal), fixture.measure(QueueType::Graphics, kHigh, equal), &Counters::nativeRecordCalls) == 1
    );
    CHECK(
        perOperation(fixture.measure(QueueType::Graphics, kLow, unequal), fixture.measure(QueueType::Graphics, kHigh, unequal), &Counters::nativeRecordCalls) ==
        5
    );
}

//======================================================================================================================
TEST_CASE("a copy and a Copy-to-Copy global barrier are two native calls and one native barrier, on a Graphics and on a Compute list", "[metal4]")
{
    Fixture fixture;
    const auto record = [&](CommandList list, uint32_t index) {
        list.copyBuffer(fixture.source, index * kBytes, fixture.target, index * kBytes, kBytes);
        list.barrier(std::array{ copyToCopy() });
    };
    for (const QueueType type : { QueueType::Graphics, QueueType::Compute })
    {
        const Counters low = fixture.measure(type, kLow, record);
        const Counters high = fixture.measure(type, kHigh, record);
        CHECK(perOperation(low, high, &Counters::nativeRecordCalls) == 2);
        CHECK(perOperation(low, high, &Counters::nativeBarriers) == 1);
    }
}

//======================================================================================================================
TEST_CASE("a submission of one list signalling one fence is two native submit calls", "[metal4]")
{
    Fixture fixture;
    // K lists of one copy each are recorded first; the submissions alone are counted.
    const auto submitCount = [&](uint32_t count) {
        fixture.device.beginFrame();
        std::vector<CommandList> lists;
        for (uint32_t index = 0; index < count; ++index)
        {
            CommandList list = fixture.device.acquireCommandList(QueueType::Graphics, "test.list");
            list.copyBuffer(fixture.source, index * kBytes, fixture.target, index * kBytes, kBytes);
            list.end();
            lists.push_back(list);
        }
        const Fence fence = *fixture.device.createFence(0, "test.done");
        const uint64_t before = fixture.device.counters().nativeSubmitCalls;
        for (uint32_t index = 0; index < count; ++index)
        {
            const std::array<FenceValue, 1> signal{ FenceValue{ fence, index + 1 } };
            fixture.device.queue(QueueType::Graphics).submit(std::span(&lists[index], 1), {}, signal);
        }
        const uint64_t after = fixture.device.counters().nativeSubmitCalls;
        REQUIRE(fixture.device.wait({ fence, count }, UINT64_MAX));
        fixture.device.endFrame();
        return after - before;
    };
    const uint64_t low = submitCount(kSubmitLow);
    const uint64_t high = submitCount(kLow);
    REQUIRE((high - low) % (kLow - kSubmitLow) == 0);
    // The commit and the fence signal; a submission with lists signals no queue progress.
    CHECK((high - low) / (kLow - kSubmitLow) == 2);
}
