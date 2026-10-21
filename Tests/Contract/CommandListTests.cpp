//----------------------------------------------------------------------------------------------------------------------
/// @file CommandListTests.cpp
/// @brief Tests acquiring, ending and submitting command lists and uploading frame data against chapters 03, 07 and 10.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using namespace rojoRHI;

namespace {
    constexpr std::array<std::byte, 16> kBytes = {};

    //==================================================================================================================
    Device makeDevice(uint32_t framesInFlight = 2)
    {
        Result<Device> created = createDevice(DeviceDesc{ .framesInFlight = framesInFlight });
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    bool isNull(Addr address)
    {
        return address.lo == 0 && address.hi == 0;
    }

    //==================================================================================================================
    bool sameAddress(Addr a, Addr b)
    {
        return a.lo == b.lo && a.hi == b.hi;
    }

    //==================================================================================================================
    /// Ends `list` and submits it to the queue of its type, signalling `signal`.
    void endAndSubmit(Device device, CommandList list, QueueType type = QueueType::Graphics, std::span<const FenceValue> signal = {})
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, {}, signal);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a list acquired for each queue type ends and submits to that queue", "[contract]")
{
    Device device = makeDevice();
    const Fence computeDone = *device.createFence(0, "test.computeDone");
    const Fence copyDone = *device.createFence(0, "test.copyDone");
    const std::array<FenceValue, 2> joined{ FenceValue{ computeDone, 1 }, FenceValue{ copyDone, 1 } };
    device.beginFrame();
    endAndSubmit(device, device.acquireCommandList(QueueType::Graphics, "test.list"), QueueType::Graphics);
    endAndSubmit(device, device.acquireCommandList(QueueType::Compute, "test.list"), QueueType::Compute, std::span(joined).first(1));
    endAndSubmit(device, device.acquireCommandList(QueueType::Copy, "test.list"), QueueType::Copy, std::span(joined).last(1));
    device.queue(QueueType::Graphics).submit({}, joined);
    device.endFrame();
    device.destroy(computeDone);
    device.destroy(copyDone);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("one submission takes several lists and signals its fence", "[contract]")
{
    Device device = makeDevice();
    const Fence fence = *device.createFence(0, "test.fence");
    device.beginFrame();
    std::array<CommandList, 3> lists;
    for (CommandList& list : lists)
    {
        list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.end();
    }
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    device.queue(QueueType::Graphics).submit(lists, {}, signal);
    device.endFrame();
    CHECK(device.wait({ fence, 1 }, UINT64_MAX));
    CHECK(device.completedValue(fence) == 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a list acquired without a label is accepted", "[contract]")
{
    Device device = makeDevice();
    device.beginFrame();
    endAndSubmit(device, device.acquireCommandList(QueueType::Graphics, {}));
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a frame with no lists ends", "[contract]")
{
    Device device = makeDevice();
    device.beginFrame();
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("lists are acquired again in every frame across slot reuse", "[contract]")
{
    for (const uint32_t framesInFlight : { 2u, 3u })
    {
        Device device = makeDevice(framesInFlight);
        const Fence joined = *device.createFence(0, "test.joined");
        for (uint64_t frame = 1; frame <= 8; ++frame)
        {
            device.beginFrame();
            std::array<CommandList, 4> lists;
            for (CommandList& list : lists)
            {
                list = device.acquireCommandList(QueueType::Compute, "test.list");
                list.end();
            }
            const std::array<FenceValue, 1> signal{ FenceValue{ joined, frame } };
            device.queue(QueueType::Compute).submit(lists, {}, signal);
            device.queue(QueueType::Graphics).submit({}, signal);
            device.endFrame();
        }
        device.destroy(joined);
        destroyDevice(device);
    }
}

//======================================================================================================================
TEST_CASE("frame data returns a non-null address for every upload", "[contract]")
{
    Device device = makeDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    CHECK_FALSE(isNull(list.uploadFrameData(kBytes)));
    CHECK_FALSE(isNull(list.uploadFrameData(kBytes, 16)));
    endAndSubmit(device, list);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two uploads of one frame return different addresses", "[contract]")
{
    Device device = makeDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const Addr first = list.uploadFrameData(kBytes);
    const Addr second = list.uploadFrameData(kBytes);
    CHECK_FALSE(sameAddress(first, second));
    endAndSubmit(device, list);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("lists of one frame never receive the same address", "[contract]")
{
    Device device = makeDevice();
    const Fence computeDone = *device.createFence(0, "test.computeDone");
    const std::array<FenceValue, 1> joined{ FenceValue{ computeDone, 1 } };
    device.beginFrame();
    CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
    CHECK_FALSE(sameAddress(graphics.uploadFrameData(kBytes), compute.uploadFrameData(kBytes)));
    endAndSubmit(device, graphics);
    endAndSubmit(device, compute, QueueType::Compute, joined);
    device.queue(QueueType::Graphics).submit({}, joined);
    device.endFrame();
    device.destroy(computeDone);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an upload larger than any standard arena page is accepted", "[contract]")
{
    Device device = makeDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::vector<std::byte> big(std::size_t{ 3 } << 20);
    CHECK_FALSE(isNull(list.uploadFrameData(big)));
    endAndSubmit(device, list);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("frame data is uploaded in every frame across slot reuse", "[contract]")
{
    Device device = makeDevice();
    for (int frame = 0; frame < 8; ++frame)
    {
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        for (int upload = 0; upload < 100; ++upload)
        {
            CHECK_FALSE(isNull(list.uploadFrameData(kBytes)));
        }
        endAndSubmit(device, list);
        device.endFrame();
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an address from frame data accepts offset arithmetic", "[contract]")
{
    Device device = makeDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const Addr base = list.uploadFrameData(kBytes);
    CHECK_FALSE(sameAddress(base + 8, base));
    endAndSubmit(device, list);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two lists of one frame upload from two threads and never share an address", "[contract]")
{
    Device device = makeDevice();
    device.beginFrame();
    constexpr int kUploads = 3000;
    std::array<CommandList, 2> lists = { device.acquireCommandList(QueueType::Graphics, "test.a"), device.acquireCommandList(QueueType::Graphics, "test.b") };
    std::array<std::vector<Addr>, 2> addresses;
    {
        std::array<std::jthread, 2> threads;
        for (size_t t = 0; t < threads.size(); ++t)
        {
            threads[t] = std::jthread([&, t] {
                // Sizes straddle the page size, so both lists claim pages from the shared slot while the other does.
                const std::vector<std::byte> payload(t == 0 ? 700 : 1300);
                for (int i = 0; i < kUploads; ++i)
                {
                    addresses[t].push_back(lists[t].uploadFrameData(payload));
                }
            });
        }
    }
    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (const std::vector<Addr>& list : addresses)
    {
        for (const Addr& address : list)
        {
            CHECK(seen.emplace(address.lo, address.hi).second);
        }
    }
    for (CommandList list : lists)
    {
        endAndSubmit(device, list);
    }
    device.endFrame();
    destroyDevice(device);
}
