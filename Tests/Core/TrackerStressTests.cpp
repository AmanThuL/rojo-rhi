//----------------------------------------------------------------------------------------------------------------------
/// @file TrackerStressTests.cpp
/// @brief Stresses the validation tracker with a long frame loop and with every queue submitting from its own thread.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Tracker/Tracker.h"
#include "Support/Backend.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <span>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The bytes of every buffer the cases copy and fill.
    [[maybe_unused]] constexpr uint64_t kBytes = 256;

    /// The bytes of the buffer a texture copy reads from.
    [[maybe_unused]] constexpr uint64_t kSourceBytes = 4096;

    /// The nanoseconds a fence wait may take before the case fails.
    [[maybe_unused]] constexpr uint64_t kWaitNanoseconds = 10'000'000'000;

    //==================================================================================================================
    /// Returns the description of an 8 by 8 `RGBA8Unorm` texture a copy may write and a shader may sample.
    [[maybe_unused]] TextureDesc smallTexture()
    {
        return TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .usage = TextureUsage::Sampled, .label = "test.stress.texture" };
    }

    //==================================================================================================================
    /// Records the global barrier that orders every copy and fill before it before every copy and fill after it.
    [[maybe_unused]] void copiesDone(CommandList list)
    {
        const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite } };
        list.barrier(globals);
    }

    //==================================================================================================================
    /// Records the discard of every subresource of `texture` to `CopyDst` that waits for the copy stage, as the first use of
    /// memory a copy used earlier needs.
    [[maybe_unused]] void discardAfterCopy(CommandList list, Texture texture)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .srcStage = Stage::Copy,
                                                                      .dstStage = Stage::Copy,
                                                                      .dstAccess = Access::CopyWrite,
                                                                      .oldLayout = Layout::Undefined,
                                                                      .newLayout = Layout::CopyDst } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Records the copy of the start of `source` into the whole of `texture`, which must be in `CopyDst`.
    [[maybe_unused]] void copyInto(CommandList list, Buffer source, Texture texture)
    {
        list.copyBufferToTexture(source, BufferTextureLayout{ .rowPitch = 256 }, texture, TextureRegion{ .extent = { 8, 8, 1 } });
    }

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type`, waiting on `wait` and signalling `signal`.
    [[maybe_unused]] void submitTo(
        Device device, CommandList list, QueueType type, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {}
    )
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, wait, signal);
    }

    /// The counts of tracker state a long run must keep bounded.
    struct TrackerCounts
    {
        size_t copyClocks = 0;
        size_t computeClocks = 0;
        size_t heapSegments = 0;
        size_t bufferSegments = 0;
        size_t aliasMarks = 0;
        size_t liveTextures = 0;
    };

    //==================================================================================================================
    /// Reads the counts of the state the tracker keeps for the objects of the frame loop.
    [[maybe_unused]] TrackerCounts sample(Device device, Fence copyFence, Fence computeFence, Heap heap, Buffer results, Texture placed)
    {
        core::DeviceCore& core = core::coreOf(device);
        core::Tracker& tracker = core.tracker();
        TrackerCounts counts;
        counts.copyClocks = tracker.fenceValueCount(copyFence).value_or(0);
        counts.computeClocks = tracker.fenceValueCount(computeFence).value_or(0);
        counts.heapSegments = tracker.heapSegmentCount(heap).value_or(0);
        counts.bufferSegments = tracker.segmentCount(results).value_or(0);
        counts.aliasMarks = tracker.aliasMarkCount(placed).value_or(0);
        counts.liveTextures = core.pool<Texture>().liveCount();
        return counts;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a validating device runs 100,000 frames on three queues with bounded tracker state", "[stress][.]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    constexpr uint64_t kFrames = 100'000;
    constexpr uint64_t kWarmup = 1'000;
    constexpr uint64_t kSlots = 3;
    Device device = *createDevice(DeviceDesc{ .validation = true });
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(), .label = "test.stress.swapchain" });
    const Fence copyFence = *device.createFence(0, "test.stress.copy");
    const Fence computeFence = *device.createFence(0, "test.stress.compute");
    const Heap heap = *device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.stress.heap" });
    const Buffer placedBuffer = *device.createBuffer(
        BufferDesc{ .size = kBytes, .usage = BufferUsage::ShaderWrite, .label = "test.stress.placed.buffer" }, {}, Placement{ .heap = heap }
    );
    const Texture placedTexture = *device.createTexture(smallTexture(), {}, Placement{ .heap = heap });
    const Buffer source = *device.createBuffer(BufferDesc{ .size = kSourceBytes, .label = "test.stress.source" });
    std::array<Buffer, kSlots> uploads{};
    std::array<Buffer, kSlots> results{};
    for (size_t slot = 0; slot < kSlots; ++slot)
    {
        uploads[slot] = *device.createBuffer(BufferDesc{ .size = kBytes, .usage = BufferUsage::ShaderWrite, .label = "test.stress.upload" });
        results[slot] = *device.createBuffer(BufferDesc{ .size = kBytes, .usage = BufferUsage::ShaderWrite, .label = "test.stress.result" });
    }
    uint64_t frames = 0;
    TrackerCounts warm;
    for (uint64_t frame = 1; frame <= kFrames; ++frame)
    {
        const size_t slot = frame % kSlots;
        device.beginFrame();
        // The copy queue writes this slot's upload, three frames after the graphics queue last read it.
        CommandList copy = device.acquireCommandList(QueueType::Copy, "test.stress.copy");
        copy.copyBuffer(source, 0, uploads[slot], 0, kBytes);
        const std::array<FenceValue, 1> copied{ FenceValue{ copyFence, frame } };
        submitTo(device, copy, QueueType::Copy, {}, copied);
        CommandList compute = device.acquireCommandList(QueueType::Compute, "test.stress.compute");
        compute.fillBuffer(results[slot], 0, kBytes, static_cast<uint32_t>(frame));
        const std::array<FenceValue, 1> computed{ FenceValue{ computeFence, frame } };
        submitTo(device, compute, QueueType::Compute, {}, computed);
        // The graphics queue reads what the copy queue wrote and overwrites what the compute queue wrote, behind the waits.
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.stress.graphics");
        copiesDone(list);
        list.copyBuffer(uploads[slot], 0, results[slot], 0, kBytes);
        const Texture transient = *device.createTexture(smallTexture());
        test::transition(list, transient, Layout::Undefined, Layout::CopyDst);
        copyInto(list, source, transient);
        test::transition(list, transient, Layout::CopyDst, Layout::ShaderRead);
        // The placed buffer and texture take turns on the same heap bytes, each first use behind the barrier aliasing needs.
        if (frame % 2 == 0)
        {
            copiesDone(list);
            list.fillBuffer(placedBuffer, 0, kBytes, static_cast<uint32_t>(frame));
        }
        else
        {
            discardAfterCopy(list, placedTexture);
            copyInto(list, source, placedTexture);
        }
        const Texture image = swapchain.acquire();
        test::transition(list, image, Layout::Undefined, Layout::ColorAttachment);
        test::transition(list, image, Layout::ColorAttachment, Layout::Present);
        const std::array<FenceValue, 2> joined{ copied[0], computed[0] };
        submitTo(device, list, QueueType::Graphics, joined);
        device.queue(QueueType::Graphics).present(swapchain);
        device.destroy(transient);
        device.endFrame();
        ++frames;
        if (frame == kWarmup)
        {
            warm = sample(device, copyFence, computeFence, heap, results[0], placedTexture);
        }
    }
    const TrackerCounts end = sample(device, copyFence, computeFence, heap, results[0], placedTexture);
    CHECK(frames == kFrames);
    REQUIRE(device.wait(FenceValue{ copyFence, kFrames }, kWaitNanoseconds));
    REQUIRE(device.wait(FenceValue{ computeFence, kFrames }, kWaitNanoseconds));
    CHECK(warm.copyClocks > 0);
    CHECK(warm.heapSegments > 0);
    // A fence signalled once a frame keeps at most framesInFlight + 1 values.
    CHECK(end.copyClocks <= 4);
    CHECK(end.computeClocks <= 4);
    CHECK(end.copyClocks <= warm.copyClocks);
    CHECK(end.computeClocks <= warm.computeClocks);
    CHECK(end.heapSegments <= warm.heapSegments);
    CHECK(end.bufferSegments <= warm.bufferSegments);
    CHECK(end.aliasMarks <= warm.aliasMarks);
    CHECK(end.liveTextures <= warm.liveTextures);
    device.destroy(swapchain);
    for (size_t slot = 0; slot < kSlots; ++slot)
    {
        device.destroy(uploads[slot]);
        device.destroy(results[slot]);
    }
    device.destroy(source);
    device.destroy(placedTexture);
    device.destroy(placedBuffer);
    device.destroy(heap);
    device.destroy(copyFence);
    device.destroy(computeFence);
    destroyDevice(device);
#endif
}

//======================================================================================================================
TEST_CASE("every queue submits from its own thread while another thread reads their fences", "[stress][.]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    constexpr uint64_t kBatches = 16;
    constexpr uint64_t kRounds = 1'000;
    constexpr uint64_t kWaitEvery = 8;
    constexpr std::array<QueueType, 3> kQueues{ QueueType::Graphics, QueueType::Compute, QueueType::Copy };
    Device device = *createDevice(DeviceDesc{ .validation = true });
    const Buffer common = *device.createBuffer(BufferDesc{ .size = kSourceBytes, .label = "test.stress.common" });
    std::array<Buffer, 3> destinations{};
    std::array<Fence, 3> fences{};
    std::array<std::atomic<uint64_t>, 3> published{};
    std::array<uint64_t, 3> submitted{};
    for (size_t index = 0; index < kQueues.size(); ++index)
    {
        destinations[index] = *device.createBuffer(BufferDesc{ .size = kBytes, .label = "test.stress.destination" });
        fences[index] = *device.createFence(0, "test.stress.fence");
    }
    // A Copy list takes no barrier out of Undefined, so the Copy queue's thread copies into a texture the Graphics queue
    // moves to General before the first batch begins, and waits on that move in its first round.
    const Texture general = *device.createTexture(smallTexture());
    const Fence moved = *device.createFence(0, "test.stress.moved");
    const size_t texturesBefore = core::coreOf(device).pool<Texture>().liveCount();
    // Each thread creates its own transient texture and copies into it, or into the General texture on the Copy queue, and
    // into its own buffer, and signals its own fence; every kWaitEvery rounds it also waits on a value another queue's thread
    // had already submitted for signal, so no wait precedes its signal.
    const auto work = [&](size_t index, uint64_t batch, std::latch& start) {
        start.arrive_and_wait();
        for (uint64_t round = 1; round <= kRounds; ++round)
        {
            const uint64_t value = batch * kRounds + round;
            const Texture texture = *device.createTexture(smallTexture());
            CommandList list = device.acquireCommandList(kQueues[index], "test.stress.list");
            const bool copyQueue = kQueues[index] == QueueType::Copy;
            if (!copyQueue)
            {
                test::transition(list, texture, Layout::Undefined, Layout::CopyDst);
            }
            copyInto(list, common, copyQueue ? general : texture);
            copiesDone(list);
            list.copyBuffer(common, 0, destinations[index], 0, kBytes);
            std::array<FenceValue, 2> wait{};
            size_t waits = 0;
            if (copyQueue && batch == 0 && round == 1)
            {
                wait[waits++] = FenceValue{ moved, 1 };
            }
            if (round % kWaitEvery == 0)
            {
                const size_t other = (index + 1) % kQueues.size();
                const uint64_t reached = published[other].load();
                if (reached > 0)
                {
                    wait[waits++] = FenceValue{ fences[other], reached };
                }
            }
            const std::array<FenceValue, 1> signal{ FenceValue{ fences[index], value } };
            submitTo(device, list, kQueues[index], std::span<const FenceValue>(wait.data(), waits), signal);
            published[index].store(value);
            device.destroy(texture);
            ++submitted[index];
        }
    };
    for (uint64_t batch = 0; batch < kBatches; ++batch)
    {
        device.beginFrame();
        if (batch == 0)
        {
            CommandList move = device.acquireCommandList(QueueType::Graphics, "test.stress.move");
            test::transition(move, general, Layout::Undefined, Layout::General);
            const std::array<FenceValue, 1> signal{ FenceValue{ moved, 1 } };
            submitTo(device, move, QueueType::Graphics, {}, signal);
        }
        std::atomic<bool> submitting{ true };
        const auto observe = [&] {
            while (submitting.load())
            {
                for (const Fence fence : fences)
                {
                    const uint64_t reached = device.completedValue(fence);
                    static_cast<void>(device.wait(FenceValue{ fence, reached }, 0));
                }
            }
        };
        std::thread observer(observe);
        // The three submitters begin together.
        std::latch start(static_cast<std::ptrdiff_t>(kQueues.size()));
        std::vector<std::thread> queues;
        for (size_t index = 0; index < kQueues.size(); ++index)
        {
            queues.emplace_back(work, index, batch, std::ref(start));
        }
        for (std::thread& queue : queues)
        {
            queue.join();
        }
        submitting.store(false);
        observer.join();
        // The frame's graphics work joins the compute and copy work before it ends.
        const uint64_t last = (batch + 1) * kRounds;
        const std::array<FenceValue, 2> joined{ FenceValue{ fences[1], last }, FenceValue{ fences[2], last } };
        device.queue(QueueType::Graphics).submit({}, joined);
        device.endFrame();
    }
    for (const Fence fence : fences)
    {
        REQUIRE(device.wait(FenceValue{ fence, kBatches * kRounds }, kWaitNanoseconds));
        CHECK(device.completedValue(fence) == kBatches * kRounds);
    }
    CHECK(submitted[0] + submitted[1] + submitted[2] == 3 * kBatches * kRounds);
    CHECK(core::coreOf(device).pool<Texture>().liveCount() == texturesBefore);
    device.destroy(general);
    device.destroy(moved);
    device.destroy(common);
    for (size_t index = 0; index < kQueues.size(); ++index)
    {
        device.destroy(destinations[index]);
        device.destroy(fences[index]);
    }
    destroyDevice(device);
#endif
}
