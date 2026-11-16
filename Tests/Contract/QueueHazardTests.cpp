//----------------------------------------------------------------------------------------------------------------------
/// @file QueueHazardTests.cpp
/// @brief Tests that the validation tracker accepts uses on different queues that a fence, or the CPU's observation, orders.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// The bytes every test writes and reads.
    [[maybe_unused]] constexpr uint64_t kBytes = 256;

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`, waiting on `wait` and signalling `signal`.
    [[maybe_unused]] void submitTo(
        Device device, CommandList list, QueueType type, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {}
    )
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, wait, signal);
    }

    //==================================================================================================================
    /// Creates a buffer of `kBytes` bytes that a fill may write.
    [[maybe_unused]] Buffer storage(PassKit& kit)
    {
        return kit.buffer(BufferUsage::ShaderWrite, kBytes);
    }

    //==================================================================================================================
    /// Creates a `Memory::Gpu` heap of one mebibyte.
    [[maybe_unused]] Heap makeHeap(PassKit& kit)
    {
        return *kit.device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.heap" });
    }

    //==================================================================================================================
    /// Creates a buffer of `kBytes` bytes that a fill may write, placed at the start of `heap`.
    [[maybe_unused]] Buffer placedBuffer(PassKit& kit, Heap heap)
    {
        const BufferDesc desc{ .size = kBytes, .usage = BufferUsage::ShaderWrite, .label = "test.placed.buffer" };
        return *kit.device.createBuffer(desc, {}, Placement{ .heap = heap });
    }

    //==================================================================================================================
    /// Joins the compute and copy work into the frame's graphics work, ends the frame and destroys the device of `kit`.
    [[maybe_unused]] void close(PassKit& kit)
    {
        kit.joinQueues();
        kit.device.endFrame();
        destroyDevice(kit.device);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a buffer written on Graphics is read on Compute after a fence wait", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = storage(kit);
    const Buffer other = storage(kit);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> written{ FenceValue{ fence, 1 } };
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBuffer(buffer, 0, other, 0, kBytes);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, written);
    submitTo(kit.device, compute, QueueType::Compute, written);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a buffer read on Compute is written on Graphics after a fence wait", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = storage(kit);
    const Buffer other = storage(kit);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> read{ FenceValue{ fence, 1 } };
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBuffer(buffer, 0, other, 0, kBytes);
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    submitTo(kit.device, compute, QueueType::Compute, {}, read);
    submitTo(kit.device, kit.list, QueueType::Graphics, read);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("placed buffers over the same bytes are used on two queues after a fence wait", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer first = placedBuffer(kit, heap);
    const Buffer second = placedBuffer(kit, heap);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> used{ FenceValue{ fence, 1 } };
    kit.list.fillBuffer(first, 0, kBytes, 0);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.fillBuffer(second, 0, kBytes, 1);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, used);
    submitTo(kit.device, compute, QueueType::Compute, used);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a submission is ordered through an earlier submission's wait on the same queue", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = storage(kit);
    const Buffer other = storage(kit);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> written{ FenceValue{ fence, 1 } };
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, written);
    // The first Compute submission only waits; the second, later on the queue, reads with no wait of its own.
    kit.device.queue(QueueType::Compute).submit({}, written);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBuffer(buffer, 0, other, 0, kBytes);
    submitTo(kit.device, compute, QueueType::Compute);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a CPU wait that returns true orders the work before it ahead of a submission on another queue", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = storage(kit);
    const Buffer other = storage(kit);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> written{ FenceValue{ fence, 1 } };
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, written);
    REQUIRE(kit.device.wait(written[0], UINT64_MAX));
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBuffer(buffer, 0, other, 0, kBytes);
    submitTo(kit.device, compute, QueueType::Compute);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a completed value the CPU read orders the work before it ahead of a submission on another queue", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = storage(kit);
    const Buffer other = storage(kit);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> written{ FenceValue{ fence, 1 } };
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, written);
    // completedValue never blocks, so the read repeats until the submission reaches its value or a bounded time passes.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (kit.device.completedValue(fence) < 1 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    REQUIRE(kit.device.completedValue(fence) >= 1);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBuffer(buffer, 0, other, 0, kBytes);
    submitTo(kit.device, compute, QueueType::Compute);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a frame's graphics work is complete for a submission on another queue once beginFrame retires its slot", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = storage(kit);
    const Buffer other = storage(kit);
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    submitTo(kit.device, kit.list, QueueType::Graphics);
    kit.device.endFrame();
    // The device keeps three frames in flight: the fourth frame's beginFrame retires the first.
    for (int frame = 0; frame < 3; ++frame)
    {
        if (frame != 0)
        {
            kit.device.endFrame();
        }
        kit.device.beginFrame();
    }
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBuffer(buffer, 0, other, 0, kBytes);
    submitTo(kit.device, compute, QueueType::Compute);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a fence round trip through another queue orders a same-queue pair with no barrier", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = storage(kit);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> written{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> returned{ FenceValue{ fence, 2 } };
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, written);
    kit.device.queue(QueueType::Compute).submit({}, written, returned);
    CommandList later = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
    later.fillBuffer(buffer, 0, kBytes, 1);
    submitTo(kit.device, later, QueueType::Graphics, returned);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("reads of a buffer on two queues need no fence", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = storage(kit);
    const Buffer graphicsCopy = storage(kit);
    const Buffer computeCopy = storage(kit);
    kit.list.copyBuffer(buffer, 0, graphicsCopy, 0, kBytes);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBuffer(buffer, 0, computeCopy, 0, kBytes);
    submitTo(kit.device, kit.list, QueueType::Graphics);
    submitTo(kit.device, compute, QueueType::Compute);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("one fence signalled from two queues orders a wait after every value at least it", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer graphicsWritten = storage(kit);
    const Buffer computeWritten = storage(kit);
    const Buffer first = storage(kit);
    const Buffer second = storage(kit);
    const Buffer third = storage(kit);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> one{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> two{ FenceValue{ fence, 2 } };
    kit.list.fillBuffer(graphicsWritten, 0, kBytes, 0);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.fillBuffer(computeWritten, 0, kBytes, 1);
    CommandList copyFirst = kit.device.acquireCommandList(QueueType::Copy, "test.copy.first");
    copyFirst.copyBuffer(graphicsWritten, 0, first, 0, kBytes);
    CommandList copySecond = kit.device.acquireCommandList(QueueType::Copy, "test.copy.second");
    copySecond.copyBuffer(graphicsWritten, 0, second, 0, kBytes);
    copySecond.copyBuffer(computeWritten, 0, third, 0, kBytes);
    // Compute signals the fence's second value after waiting on its first, so a wait on either follows the Graphics fill.
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, one);
    submitTo(kit.device, compute, QueueType::Compute, one, two);
    submitTo(kit.device, copyFirst, QueueType::Copy, one);
    submitTo(kit.device, copySecond, QueueType::Copy, two);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("three queues submit while another thread waits on and reads their fences", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    constexpr uint64_t kRounds = 32;
    Device device = *createDevice(DeviceDesc{ .validation = true });
    device.beginFrame();
    constexpr std::array<QueueType, 3> kQueues{ QueueType::Graphics, QueueType::Compute, QueueType::Copy };
    std::array<Buffer, 3> sources{};
    std::array<Buffer, 3> buffers{};
    std::array<Fence, 3> fences{};
    for (size_t index = 0; index < kQueues.size(); ++index)
    {
        sources[index] = *device.createBuffer(BufferDesc{ .size = kBytes, .label = "test.source" });
        buffers[index] = *device.createBuffer(BufferDesc{ .size = kBytes, .label = "test.buffer" });
        fences[index] = *device.createFence(0, "test.fence");
    }
    std::atomic<bool> submitting{ true };
    // Each queue copies into its own buffer every round, behind a barrier from the previous round's copy, and signals its
    // fence.
    const auto work = [&](size_t index) {
        for (uint64_t round = 1; round <= kRounds; ++round)
        {
            CommandList list = device.acquireCommandList(kQueues[index], "test.list");
            const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{
                .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite } };
            list.barrier(globals);
            list.copyBuffer(sources[index], 0, buffers[index], 0, kBytes);
            const std::array<FenceValue, 1> signal{ FenceValue{ fences[index], round } };
            list.end();
            const std::array<CommandList, 1> lists{ list };
            device.queue(kQueues[index]).submit(lists, {}, signal);
        }
    };
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
    std::vector<std::thread> queues;
    for (size_t index = 0; index < kQueues.size(); ++index)
    {
        queues.emplace_back(work, index);
    }
    for (std::thread& queue : queues)
    {
        queue.join();
    }
    submitting.store(false);
    observer.join();
    for (const Fence fence : fences)
    {
        REQUIRE(device.wait(FenceValue{ fence, kRounds }, 10'000'000'000));
        CHECK(device.completedValue(fence) == kRounds);
    }
    // The frame's graphics work joins the compute and copy work before it ends.
    const std::array<FenceValue, 2> joined{ FenceValue{ fences[1], kRounds }, FenceValue{ fences[2], kRounds } };
    device.queue(QueueType::Graphics).submit({}, joined);
    device.endFrame();
    for (size_t index = 0; index < kQueues.size(); ++index)
    {
        device.destroy(sources[index]);
        device.destroy(buffers[index]);
        device.destroy(fences[index]);
    }
    destroyDevice(device);
#endif
}

//======================================================================================================================
TEST_CASE("a placed buffer's first use after another buffer's use the CPU observed complete needs no barrier", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer first = placedBuffer(kit, heap);
    const Buffer second = placedBuffer(kit, heap);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> used{ FenceValue{ fence, 1 } };
    kit.list.fillBuffer(first, 0, kBytes, 0);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, used);
    REQUIRE(kit.device.wait(used[0], UINT64_MAX));
    CommandList later = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
    later.fillBuffer(second, 0, kBytes, 1);
    submitTo(kit.device, later, QueueType::Graphics);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a placed texture's discard after a use a fence orders before it needs no source covering that use", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Buffer buffer = placedBuffer(kit, heap);
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { PassKit::kSize, PassKit::kSize, 1 }, .usage = TextureUsage::Storage, .label = "test.placed" },
        {},
        Placement{ .heap = heap }
    );
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> filled{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> returned{ FenceValue{ fence, 2 } };
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, filled);
    kit.device.queue(QueueType::Compute).submit({}, filled, returned);
    // The discard still starts from Undefined, but its source need not cover the fill the fence round trip ordered before it.
    CommandList later = kit.device.acquireCommandList(QueueType::Graphics, "test.later");
    const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                  .srcStage = Stage::ComputeShader,
                                                                  .dstStage = Stage::Copy,
                                                                  .dstAccess = Access::CopyWrite,
                                                                  .oldLayout = Layout::Undefined,
                                                                  .newLayout = Layout::CopyDst } };
    later.barrier({}, {}, barriers);
    later.clearTexture(texture, {}, {});
    submitTo(kit.device, later, QueueType::Graphics, returned);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a new texture uploaded on Copy is read on Graphics after a fence wait", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture uploaded = kit.texture();
    const Buffer staging = storage(kit);
    const Buffer readback = kit.buffer(BufferUsage::ShaderWrite, 64 * 1024);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> uploadedValue{ FenceValue{ fence, 1 } };
    const TextureRegion region{ .extent = { PassKit::kSize, 1, 1 } };
    // A Copy list finds a new texture in General, so its first barrier on it keeps General, the only texture barrier the
    // Copy list may record, and needs no barrier on another queue first.
    CommandList copy = kit.device.acquireCommandList(QueueType::Copy, "test.copy");
    const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = uploaded,
                                                                  .srcStage = Stage::All,
                                                                  .dstStage = Stage::All,
                                                                  .dstAccess = Access::CopyRead | Access::CopyWrite,
                                                                  .oldLayout = Layout::General,
                                                                  .newLayout = Layout::General } };
    copy.barrier({}, {}, barriers);
    copy.copyBufferToTexture(staging, BufferTextureLayout{ .rowPitch = 256 }, uploaded, region);
    kit.list.copyTextureToBuffer(uploaded, region, readback, BufferTextureLayout{ .rowPitch = 256 });
    submitTo(kit.device, copy, QueueType::Copy, {}, uploadedValue);
    submitTo(kit.device, kit.list, QueueType::Graphics, uploadedValue);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a texture is moved on Compute after a fence orders the Graphics reads before it", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture shared = kit.texture();
    const Buffer graphicsCopy = kit.buffer(BufferUsage::ShaderWrite, 64 * 1024);
    const Buffer computeCopy = kit.buffer(BufferUsage::ShaderWrite, 64 * 1024);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> read{ FenceValue{ fence, 1 } };
    const TextureRegion region{ .extent = { PassKit::kSize, 1, 1 } };
    const std::array<TextureBarrier, 1> toGeneral{ TextureBarrier{ .texture = shared,
                                                                   .srcStage = Stage::All,
                                                                   .dstStage = Stage::All,
                                                                   .dstAccess = Access::CopyRead,
                                                                   .oldLayout = Layout::Undefined,
                                                                   .newLayout = Layout::General } };
    kit.list.barrier({}, {}, toGeneral);
    kit.list.copyTextureToBuffer(shared, region, graphicsCopy, BufferTextureLayout{ .rowPitch = 256 });
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, read);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    const std::array<TextureBarrier, 1> toCopySrc{ TextureBarrier{ .texture = shared,
                                                                   .srcStage = Stage::All,
                                                                   .dstStage = Stage::All,
                                                                   .dstAccess = Access::CopyRead,
                                                                   .oldLayout = Layout::General,
                                                                   .newLayout = Layout::CopySrc } };
    compute.barrier({}, {}, toCopySrc);
    compute.copyTextureToBuffer(shared, region, computeCopy, BufferTextureLayout{ .rowPitch = 256 });
    submitTo(kit.device, compute, QueueType::Compute, read);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a buffer is filled on Compute over a placed texture Graphics moved, after a fence wait", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = makeHeap(kit);
    const Texture placed = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { PassKit::kSize, PassKit::kSize, 1 }, .usage = TextureUsage::Storage, .label = "test.placed" },
        {},
        Placement{ .heap = heap }
    );
    const Buffer buffer = placedBuffer(kit, heap);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
    test::transition(kit.list, placed, Layout::Undefined, Layout::General);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, moved);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.fillBuffer(buffer, 0, kBytes, 1);
    submitTo(kit.device, compute, QueueType::Compute, moved);
    close(kit);
#endif
}
