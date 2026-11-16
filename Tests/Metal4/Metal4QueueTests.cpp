//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4QueueTests.cpp
/// @brief Tests Metal 4 fences and submission against the native events and queues they drive.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Metal4Common.h"
#include "Metal4Device.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Metal/Metal.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// The native event `fence` wraps.
    MTL::SharedEvent* eventOf(Device device, Fence fence)
    {
        return static_cast<MTL::SharedEvent*>(DeviceImpl::of(device).core.pool<Fence>().native(fence));
    }

    //==================================================================================================================
    /// The label of `event`, read inside a pool because the getter autoreleases.
    std::string labelOf(MTL::SharedEvent* event)
    {
        const metal4::AutoreleasePool pool;
        return metal4::toStdString(event->label());
    }

    //==================================================================================================================
    /// Submits no list to the queue of `type` on `device`, only the signals of `signal`.
    void signalOn(Device device, QueueType type, std::span<const FenceValue> signal)
    {
        device.queue(type).submit({}, {}, signal);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a fence's event starts at the fence's initial value and carries its label", "[metal4]")
{
    Device device = *createDevice();
    const Fence named = *device.createFence(7, "test.fence");
    MTL::SharedEvent* event = eventOf(device, named);
    REQUIRE(event != nullptr);
    CHECK(event->signaledValue() == 7);
    CHECK(labelOf(event) == "test.fence");
    const Fence unnamed = *device.createFence(0, "");
    CHECK(eventOf(device, unnamed)->signaledValue() == 0);
    CHECK(labelOf(eventOf(device, unnamed)) == "rojorhi.fence.unnamed");
    CHECK(eventOf(device, unnamed) != event);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("completedValue reports what the event has reached once the GPU signals it", "[metal4]")
{
    Device device = *createDevice();
    const Fence fence = *device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 5 } };
    signalOn(device, QueueType::Compute, signal);
    REQUIRE(eventOf(device, fence)->waitUntilSignaledValue(5, 10'000));
    const Counters before = device.counters();
    CHECK(device.completedValue(fence) == 5);
    CHECK(device.counters().nativeHostCalls == before.nativeHostCalls + 1);
    // The value the call observed is the core's too: a poll needs no native wait.
    const Counters polled = device.counters();
    CHECK(device.wait({ fence, 5 }, 0));
    CHECK(device.counters().nativeHostCalls == polled.nativeHostCalls);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Release build records no submitted fence value, which only the checks read", "[metal4]")
{
    Device device = *createDevice();
    const Fence fence = *device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 5 } };
    signalOn(device, QueueType::Compute, signal);
    REQUIRE(eventOf(device, fence)->waitUntilSignaledValue(5, 10'000));
    uint64_t submitted = 0;
    DeviceImpl::of(device).core.pool<Fence>().update(fence, [&](const core::FenceRecord& record) { submitted = record.submitted; });
#ifdef NDEBUG
    // Neither checkSubmit nor recordSignals touched the record: Release takes no fence pool lock to submit.
    CHECK(submitted == 0);
#else
    CHECK(submitted == 5);
#endif
    // The completed value, which Release reads, still comes from the event.
    CHECK(device.completedValue(fence) == 5);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("submit commits the residency set only when an allocation joined or left it", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    Queue queue = device.queue(QueueType::Graphics);
    // Device creation leaves its ID tables to the first commit.
    const Counters created = device.counters();
    queue.submit({});
    const Counters first = device.counters();
    CHECK(first.nativeHostCalls == created.nativeHostCalls + 1);
    queue.submit({});
    const Counters unchanged = device.counters();
    CHECK(unchanged.nativeHostCalls == first.nativeHostCalls);
    const Buffer buffer = *device.createBuffer({ .size = 256 });
    const Counters joined = device.counters();
    queue.submit({});
    CHECK(device.counters().nativeHostCalls == joined.nativeHostCalls + 1);
    device.destroy(buffer);
    for (int frame = 0; frame < 3; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    const Counters left = device.counters();
    queue.submit({});
    CHECK(device.counters().nativeHostCalls == left.nativeHostCalls + 1);
    const Counters settled = device.counters();
    queue.submit({});
    CHECK(device.counters().nativeHostCalls == settled.nativeHostCalls);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Device::wait with a finite timeout returns false for a value nothing will signal", "[metal4]")
{
    Device device = *createDevice();
    const Fence fence = *device.createFence(3, "test.fence");
    const Counters before = device.counters();
    CHECK_FALSE(device.wait({ fence, 4 }, 1'000'000));
    CHECK(device.counters().nativeHostCalls == before.nativeHostCalls + 1);
    CHECK(device.completedValue(fence) == 3);
    // A value already reached is answered by the core, with no native wait.
    const Counters reached = device.counters();
    CHECK(device.wait({ fence, 3 }, 1'000'000));
    CHECK(device.counters().nativeHostCalls == reached.nativeHostCalls);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a submission's waits and signals each count one native call", "[metal4]")
{
    Device device = *createDevice();
    const Fence a = *device.createFence(2, "test.a");
    const Fence b = *device.createFence(0, "test.b");
    const Fence c = *device.createFence(0, "test.c");
    const Fence d = *device.createFence(0, "test.d");
    Queue queue = device.queue(QueueType::Copy);
    const Counters start = device.counters();
    queue.submit({});
    const Counters empty = device.counters();
    const std::array<FenceValue, 2> wait{ FenceValue{ a, 2 }, FenceValue{ a, 1 } };
    const std::array<FenceValue, 3> signal{ FenceValue{ b, 1 }, FenceValue{ c, 1 }, FenceValue{ d, 1 } };
    queue.submit({}, wait, signal);
    const Counters carried = device.counters();
    // A submission with no list commits no command buffer: an empty one makes no native call.
    CHECK(empty.nativeSubmitCalls == start.nativeSubmitCalls);
    // Two waits, three signals and the signal of the queue's progress event, which releases the events it holds.
    CHECK(carried.nativeSubmitCalls == empty.nativeSubmitCalls + 6);
    for (const Fence fence : { b, c, d })
    {
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a queue wait on a fence's initial value passes at once on the GPU", "[metal4]")
{
    Device device = *createDevice();
    const Fence started = *device.createFence(4, "test.started");
    const Fence done = *device.createFence(0, "test.done");
    const std::array<FenceValue, 1> wait{ FenceValue{ started, 4 } };
    const std::array<FenceValue, 1> signal{ FenceValue{ done, 1 } };
    device.queue(QueueType::Copy).submit({}, wait, signal);
    CHECK(device.wait({ done, 1 }, UINT64_MAX));
    CHECK(eventOf(device, done)->signaledValue() == 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a queue waits on the GPU for a value another queue signals", "[metal4]")
{
    Device device = *createDevice();
    const Fence first = *device.createFence(0, "test.first");
    const Fence second = *device.createFence(0, "test.second");
    const std::array<FenceValue, 1> firstSignal{ FenceValue{ first, 1 } };
    const std::array<FenceValue, 1> secondSignal{ FenceValue{ second, 1 } };
    // The compute queue waits for a value submitted for signal on the copy queue before signalling its own fence.
    signalOn(device, QueueType::Copy, firstSignal);
    device.queue(QueueType::Compute).submit({}, firstSignal, secondSignal);
    CHECK(device.wait({ second, 1 }, UINT64_MAX));
    CHECK(eventOf(device, first)->signaledValue() >= 1);
    CHECK(device.completedValue(first) == 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("lists of a frame submitted on three queues from three threads join the graphics queue", "[metal4]")
{
    Device device = *createDevice();
    const std::array<QueueType, 3> types{ QueueType::Graphics, QueueType::Compute, QueueType::Copy };
    std::array<Fence, 3> fences{};
    for (size_t i = 0; i < fences.size(); ++i)
    {
        fences[i] = *device.createFence(0, "test.fence");
    }
    for (uint64_t frame = 1; frame <= 8; ++frame)
    {
        device.beginFrame();
        std::vector<std::thread> threads;
        for (size_t i = 0; i < types.size(); ++i)
        {
            threads.emplace_back([&, i] {
                CommandList list = device.acquireCommandList(types[i], "test.list");
                list.end();
                const std::array<CommandList, 1> lists{ list };
                const std::array<FenceValue, 1> signal{ FenceValue{ fences[i], frame } };
                device.queue(types[i]).submit(lists, {}, signal);
            });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }
        // The frame's compute and copy work joins the graphics queue before the frame ends.
        const std::array<FenceValue, 2> join{ FenceValue{ fences[1], frame }, FenceValue{ fences[2], frame } };
        device.queue(QueueType::Graphics).submit({}, join);
        device.endFrame();
    }
    for (const Fence fence : fences)
    {
        CHECK(device.wait({ fence, 8 }, UINT64_MAX));
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("destroyDevice straight after submissions drains every signal first", "[metal4]")
{
    for (int round = 0; round < 20; ++round)
    {
        Device device = *createDevice();
        const Fence fence = *device.createFence(0, "test.fence");
        MTL::SharedEvent* event = eventOf(device, fence);
        // Kept alive past the device, so the value the drain reached can be read.
        event->retain();
        for (uint64_t value = 1; value <= 3; ++value)
        {
            const std::array<FenceValue, 1> signal{ FenceValue{ fence, value } };
            signalOn(device, static_cast<QueueType>(value - 1), signal);
        }
        destroyDevice(device);
        CHECK(event->signaledValue() == 3);
        const metal4::AutoreleasePool pool;
        event->release();
    }
}

//======================================================================================================================
TEST_CASE("a queue's native wait holds back its signal until another queue signals the value", "[metal4]")
{
    Device device = *createDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const Fence x = *device.createFence(0, "test.x");
    const Fence y = *device.createFence(0, "test.y");
    MTL::SharedEvent* raw = impl.device->newSharedEvent();
    REQUIRE(raw != nullptr);
    // The copy queue cannot signal x before the test releases it.
    impl.queues[static_cast<size_t>(QueueType::Copy)].native->wait(raw, 1);
    const std::array<FenceValue, 1> xSignal{ FenceValue{ x, 1 } };
    const std::array<FenceValue, 1> ySignal{ FenceValue{ y, 1 } };
    signalOn(device, QueueType::Copy, xSignal);
    device.queue(QueueType::Compute).submit({}, xSignal, ySignal);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(eventOf(device, y)->signaledValue() == 0);
    CHECK(device.completedValue(y) == 0);
    raw->setSignaledValue(1);
    CHECK(device.wait({ y, 1 }, UINT64_MAX));
    CHECK(device.completedValue(x) == 1);
    destroyDevice(device);
    const metal4::AutoreleasePool pool;
    raw->release();
}

//======================================================================================================================
TEST_CASE("a fence destroyed while a fence-only submission outside a frame uses it keeps its event until the queue passes it", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    DeviceImpl& impl = DeviceImpl::of(device);
    QueueImpl& compute = impl.queues[static_cast<size_t>(QueueType::Compute)];
    const Fence fence = *device.createFence(0, "test.fence");
    MTL::SharedEvent* event = eventOf(device, fence);
    MTL::SharedEvent* raw = impl.device->newSharedEvent();
    REQUIRE(raw != nullptr);
    compute.native->wait(raw, 1);
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    signalOn(device, QueueType::Compute, signal);
    device.destroy(fence);
    // The fence retires with the second frame slot after the call; the blocked queue still waits to signal its event.
    for (int frame = 0; frame < 3; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    CHECK(impl.core.pool<Fence>().native(fence) == nullptr);
    {
        std::lock_guard lock(impl.retiringMutex);
        REQUIRE(impl.retiringEvents.size() == 1);
        CHECK(impl.retiringEvents.front().event == event);
    }
    raw->setSignaledValue(1);
    REQUIRE(compute.progress->waitUntilSignaledValue(compute.progressValue.load(), 10'000));
    CHECK(event->signaledValue() == 1);
    // The next reuse of a frame slot releases what every queue has passed.
    device.beginFrame();
    device.endFrame();
    {
        std::lock_guard lock(impl.retiringMutex);
        CHECK(impl.retiringEvents.empty());
    }
    destroyDevice(device);
    const metal4::AutoreleasePool pool;
    raw->release();
}

//======================================================================================================================
TEST_CASE("destroying a fence no submission named keeps nothing and reads no progress", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    DeviceImpl& impl = DeviceImpl::of(device);
    device.destroy(*device.createFence(0, "test.fence"));
    const Counters before = device.counters();
    for (int frame = 0; frame < 3; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    // Only the frame wait of the third frame is a host call.
    CHECK(device.counters().nativeHostCalls == before.nativeHostCalls + 1);
    {
        std::lock_guard lock(impl.retiringMutex);
        CHECK(impl.retiringEvents.empty());
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("fences destroyed without any frame keep a bounded number of events", "[metal4]")
{
    Device device = *createDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    QueueImpl& compute = impl.queues[static_cast<size_t>(QueueType::Compute)];
    size_t greatest = 0;
    for (int round = 0; round < 200; ++round)
    {
        const Fence fence = *device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        signalOn(device, QueueType::Compute, signal);
        // The queue has passed the submission, so the destroy that keeps the event may release it at once.
        REQUIRE(compute.progress->waitUntilSignaledValue(compute.progressValue.load(), 10'000));
        device.destroy(fence);
        std::lock_guard lock(impl.retiringMutex);
        greatest = std::max(greatest, impl.retiringEvents.size());
    }
    CHECK(greatest <= 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a fence-only submission signals queue progress on the compute and copy queues only", "[metal4]")
{
    const QueueType type = GENERATE(QueueType::Graphics, QueueType::Compute, QueueType::Copy);
    CAPTURE(type);
    Device device = *createDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const Fence fence = *device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    const Counters before = device.counters();
    signalOn(device, type, signal);
    const uint64_t progress = type == QueueType::Graphics ? 0 : 1;
    // The signal, then the progress signal of a queue no frame joins outside its lists.
    CHECK(device.counters().nativeSubmitCalls == before.nativeSubmitCalls + 1 + progress);
    CHECK(impl.queues[static_cast<size_t>(type)].progressValue.load() == progress);
    CHECK(device.wait({ fence, 1 }, UINT64_MAX));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("submissions with lists signal no queue progress, whatever fences they name", "[metal4]")
{
    Device device = *createDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const Fence computeDone = *device.createFence(0, "test.computeDone");
    const Fence graphicsDone = *device.createFence(0, "test.graphicsDone");
    device.beginFrame();
    CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.end();
    CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    graphics.end();
    const std::array<CommandList, 1> computeLists{ compute };
    const std::array<CommandList, 1> graphicsLists{ graphics };
    const std::array<FenceValue, 1> computeSignal{ FenceValue{ computeDone, 1 } };
    const std::array<FenceValue, 1> graphicsSignal{ FenceValue{ graphicsDone, 1 } };
    const Counters before = device.counters();
    device.queue(QueueType::Compute).submit(computeLists, {}, computeSignal);
    const Counters computed = device.counters();
    // The graphics submission joins the frame's compute work.
    device.queue(QueueType::Graphics).submit(graphicsLists, computeSignal, graphicsSignal);
    const Counters joined = device.counters();
    device.endFrame();
    // The commit and the signal, then the wait, the commit and the signal.
    CHECK(computed.nativeSubmitCalls == before.nativeSubmitCalls + 2);
    CHECK(joined.nativeSubmitCalls == computed.nativeSubmitCalls + 3);
    for (const QueueImpl& queue : impl.queues)
    {
        CHECK(queue.progressValue.load() == 0);
    }
    CHECK(device.wait({ graphicsDone, 1 }, UINT64_MAX));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a fence a frame's compute lists signal, destroyed in the frame, keeps its event until the frame retires", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    DeviceImpl& impl = DeviceImpl::of(device);
    const Fence fence = *device.createFence(0, "test.fence");
    MTL::SharedEvent* event = eventOf(device, fence);
    // The test's own reference, so the value reached can be read after the fence retires.
    event->retain();
    MTL::SharedEvent* raw = impl.device->newSharedEvent();
    REQUIRE(raw != nullptr);
    // The compute queue cannot run the frame's submission before the test releases it.
    impl.queues[static_cast<size_t>(QueueType::Compute)].native->wait(raw, 1);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Compute, "test.list");
    list.end();
    const std::array<CommandList, 1> lists{ list };
    // The submission's one signal is the one the frame joins through, so the join orders all its native calls.
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    device.queue(QueueType::Compute).submit(lists, {}, signal);
    device.queue(QueueType::Graphics).submit({}, signal);
    device.destroy(fence);
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    // The fence retires with the first frame, which the held compute queue keeps from completing: no event is kept beside
    // the fence's own reference.
    {
        std::lock_guard lock(impl.retiringMutex);
        CHECK(impl.retiringEvents.empty());
    }
    CHECK(impl.core.pool<Fence>().native(fence) == event);
    CHECK(event->signaledValue() == 0);
    raw->setSignaledValue(1);
    // The third frame waits for the first, whose graphics work waited for the compute signal, then retires the fence.
    device.beginFrame();
    CHECK(event->signaledValue() == 1);
    CHECK(impl.core.pool<Fence>().native(fence) == nullptr);
    device.endFrame();
    destroyDevice(device);
    const metal4::AutoreleasePool pool;
    event->release();
    raw->release();
}

//======================================================================================================================
TEST_CASE("a fence a graphics submission outside a frame signals keeps its event until the next frame retires", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    DeviceImpl& impl = DeviceImpl::of(device);
    const Fence fence = *device.createFence(0, "test.fence");
    MTL::SharedEvent* event = eventOf(device, fence);
    // The test's own reference, so the value reached can be read after the fence retires.
    event->retain();
    MTL::SharedEvent* raw = impl.device->newSharedEvent();
    REQUIRE(raw != nullptr);
    // The graphics queue cannot signal the fence before the test releases it.
    impl.queues[static_cast<size_t>(QueueType::Graphics)].native->wait(raw, 1);
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    signalOn(device, QueueType::Graphics, signal);
    device.destroy(fence);
    for (int frame = 0; frame < 2; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    // The fence retires with the first frame, whose frame signal follows the held fence signal on the graphics queue.
    {
        std::lock_guard lock(impl.retiringMutex);
        CHECK(impl.retiringEvents.empty());
    }
    CHECK(impl.core.pool<Fence>().native(fence) == event);
    CHECK(event->signaledValue() == 0);
    raw->setSignaledValue(1);
    device.beginFrame();
    CHECK(event->signaledValue() == 1);
    CHECK(impl.core.pool<Fence>().native(fence) == nullptr);
    device.endFrame();
    destroyDevice(device);
    const metal4::AutoreleasePool pool;
    event->release();
    raw->release();
}

//======================================================================================================================
TEST_CASE("a compute or copy submission with lists and two signals signals queue progress", "[metal4]")
{
    const QueueType type = GENERATE(QueueType::Compute, QueueType::Copy);
    CAPTURE(type);
    Device device = *createDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const Fence joined = *device.createFence(0, "test.joined");
    const Fence other = *device.createFence(0, "test.other");
    device.beginFrame();
    CommandList list = device.acquireCommandList(type, "test.list");
    list.end();
    const std::array<CommandList, 1> lists{ list };
    const std::array<FenceValue, 2> signal{ FenceValue{ joined, 1 }, FenceValue{ other, 1 } };
    const Counters before = device.counters();
    device.queue(type).submit(lists, {}, signal);
    // The commit, the two signals and the progress signal: a join through the first signal leaves the second unordered.
    CHECK(device.counters().nativeSubmitCalls == before.nativeSubmitCalls + 4);
    CHECK(impl.queues[static_cast<size_t>(type)].progressValue.load() == 1);
    const std::array<FenceValue, 1> join{ FenceValue{ joined, 1 } };
    device.queue(QueueType::Graphics).submit({}, join);
    device.endFrame();
    CHECK(device.wait({ other, 1 }, UINT64_MAX));
    destroyDevice(device);
}
