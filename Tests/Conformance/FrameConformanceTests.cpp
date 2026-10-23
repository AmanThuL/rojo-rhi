//----------------------------------------------------------------------------------------------------------------------
/// @file FrameConformanceTests.cpp
/// @brief Observes frame pacing, deferred destruction and frame data across frames of chapter 02 on a GPU.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The copy kernel's root, as `PullOracle.slang` declares it.
    struct CopyRoot
    {
        Addr source;      ///< Where the kernel reads.
        Addr destination; ///< Where it writes.
        uint32_t count;   ///< The `uint4` elements it copies.
        uint32_t pad;     ///< Padding.
    };

    /// The threads of one copy kernel group, as `PullOracle.slang` declares them.
    constexpr uint32_t kCopyGroup = 256;

    //==================================================================================================================
    /// Records on `list` the copy kernel `copy` moving `count` `uint4` elements from `source` to `destination`.
    void copyOn(CommandList list, Pipeline copy, Addr source, Addr destination, uint32_t count)
    {
        list.setPipeline(copy);
        list.pushRoot(CopyRoot{ .source = source, .destination = destination, .count = count, .pad = 0 });
        list.dispatch((count + kCopyGroup - 1) / kCopyGroup, 1, 1);
    }
} // namespace

//======================================================================================================================
TEST_CASE("beginFrame returns only after the graphics work of the frame framesInFlight earlier has completed, with framesInFlight 2 and 3", "[conformance]")
{
    test::requireGpu();
    const uint32_t framesInFlight = GENERATE(2u, 3u);
    CAPTURE(framesInFlight);
    Device device = *createDevice({ .framesInFlight = framesInFlight });
    // Chapter 10, "Frame pacing": beginFrame waits for the slot's fence from framesInFlight frames earlier, which the graphics
    // queue signals after the frame's graphics submissions, so a caller fence signalled there in frame N has reached N by then.
    const Fence fence = *device.createFence(0, "test.frame");
    Queue graphics = device.queue(QueueType::Graphics);
    for (uint32_t frame = 1; frame <= 3 * framesInFlight; ++frame)
    {
        CAPTURE(frame);
        device.beginFrame();
        if (frame > framesInFlight)
        {
            CHECK(device.completedValue(fence) >= frame - framesInFlight);
        }
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, frame } };
        graphics.submit({}, {}, signal);
        device.endFrame();
    }
    CHECK(device.wait({ fence, 3 * framesInFlight }, UINT64_MAX));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an object destroyed in frame N retires when frame N + framesInFlight begins and not before", "[conformance]")
{
    test::requireGpu();
    const uint32_t framesInFlight = GENERATE(2u, 3u);
    CAPTURE(framesInFlight);
    Device device = *createDevice({ .framesInFlight = framesInFlight });
    const Buffer buffer = *device.createBuffer({ .size = 64 });
    const Texture texture = *device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    const Texture host = *device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    const ViewIndex view = *device.createView(host, {});
    const SamplerIndex sampler = *device.createSampler({});

    device.beginFrame();
    const uint64_t before = device.counters().deferredDestructionsRetired;
    device.destroy(buffer);
    device.destroy(texture);
    device.destroy(view);
    device.destroy(sampler);
    device.endFrame();
    for (uint32_t later = 1; later < framesInFlight; ++later)
    {
        device.beginFrame();
        CAPTURE(later);
        CHECK(device.counters().deferredDestructionsRetired == before);
        // The freed view index is not handed out while its frame can still be in flight.
        const ViewIndex other = *device.createView(host, {});
        CHECK(other != view);
        device.destroy(other);
        device.endFrame();
    }
    device.beginFrame();
    CHECK(device.counters().deferredDestructionsRetired == before + 4);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("frame data of a frame still in flight keeps its bytes while later frames upload theirs, with framesInFlight 2 and 3", "[conformance]")
{
    test::requireGpu();
    const uint32_t framesInFlight = GENERATE(2u, 3u);
    CAPTURE(framesInFlight);
    const uint32_t frames = 3 * framesInFlight;
    // The `uint4` elements of the slow producer each frame runs first: 64 MiB, so a frame is likely still on the GPU while the
    // next one records and uploads.
    constexpr uint32_t kBusyElements = uint32_t{ 1 } << 22;
    constexpr uint64_t kBusyBytes = uint64_t{ kBusyElements } * 16;
    // Above any standard arena page, so the large upload takes a page of its own.
    constexpr uint64_t kLargeBytes = uint64_t{ 3 } << 20;
    constexpr uint64_t kSmallBytes = 4096;
    constexpr uint64_t kOutputBytes = kLargeBytes + kSmallBytes;

    Device device = *createDevice({ .framesInFlight = framesInFlight });
    const std::vector<std::byte> blob = test::shaderLibraryFile("PullOracle");
    const ShaderLibrary library = *device.loadShaderLibrary(blob, "PullOracle");
    const Pipeline copy = *device.createPipeline(ComputePipelineDesc{ .compute = { library, "copyMain" }, .label = "test.copy" });
    const Buffer busySource = *device.createBuffer({ .size = kBusyBytes, .memory = Memory::Upload, .label = "test.busySource" });
    const Buffer busyDestination =
        *device.createBuffer({ .size = kBusyBytes, .memory = Memory::Gpu, .usage = BufferUsage::ShaderWrite, .label = "test.busyDestination" });
    std::vector<Buffer> outputs;
    std::vector<Buffer> readbacks;
    for (uint32_t frame = 1; frame <= frames; ++frame)
    {
        outputs.push_back(*device.createBuffer({ .size = kOutputBytes, .memory = Memory::Gpu, .usage = BufferUsage::ShaderWrite, .label = "test.output" }));
        readbacks.push_back(*device.createBuffer({ .size = kOutputBytes, .memory = Memory::Readback, .label = "test.readback" }));
    }
    // The payloads are built before the loop, so recording a frame is short next to the producer it follows.
    std::vector<std::vector<std::byte>> larges;
    std::vector<std::vector<std::byte>> smalls;
    for (uint32_t frame = 1; frame <= frames; ++frame)
    {
        larges.push_back(test::pattern(kLargeBytes, frame));
        smalls.push_back(test::pattern(kSmallBytes, 1000 + frame));
    }
    const Fence fence = *device.createFence(0, "test.arena");
    Queue graphics = device.queue(QueueType::Graphics);

    // No host wait inside the loop: frame f records and uploads while earlier frames may still run, and frames f and
    // f + framesInFlight share a slot, whose arena beginFrame rewinds once the slot's earlier frame has completed.
    uint32_t overlapped = 0;
    for (uint32_t frame = 1; frame <= frames; ++frame)
    {
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.arena");
        // Every frame writes the busy destination; this orders the writes after the previous frame's on the queue.
        const std::array busyOrder{ GlobalBarrier{
            .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderWrite } };
        list.barrier(busyOrder);
        copyOn(list, copy, device.address(busySource), device.address(busyDestination), kBusyElements);
        const Addr large = list.uploadFrameData(larges[frame - 1]);
        const Addr small = list.uploadFrameData(smalls[frame - 1]);
        if (frame > 1 && device.completedValue(fence) < frame - 1)
        {
            // This frame's arena bytes were written while the frame before it was still on the GPU.
            ++overlapped;
        }
        const Buffer output = outputs[frame - 1];
        copyOn(list, copy, large, device.address(output), static_cast<uint32_t>(kLargeBytes / 16));
        copyOn(list, copy, small, device.address(output) + kLargeBytes, static_cast<uint32_t>(kSmallBytes / 16));
        const std::array written{ BufferBarrier{
            .buffer = output, .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead } };
        list.barrier({}, written);
        list.copyBuffer(output, 0, readbacks[frame - 1], 0, kOutputBytes);
        list.end();
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, frame } };
        graphics.submit(std::array{ list }, {}, signal);
        device.endFrame();
    }
    CHECK(device.wait({ fence, frames }, UINT64_MAX));

    uint32_t wrongFrames = 0;
    uint32_t firstWrongFrame = 0;
    size_t firstWrongOffset = 0;
    for (uint32_t frame = 1; frame <= frames; ++frame)
    {
        std::vector<std::byte> expected = larges[frame - 1];
        expected.insert(expected.end(), smalls[frame - 1].begin(), smalls[frame - 1].end());
        const std::byte* pointer = device.cpuPointer(readbacks[frame - 1]);
        CHECK(pointer != nullptr);
        if (pointer == nullptr || std::memcmp(pointer, expected.data(), expected.size()) == 0)
        {
            continue;
        }
        if (wrongFrames == 0)
        {
            firstWrongFrame = frame;
            while (pointer[firstWrongOffset] == expected[firstWrongOffset])
            {
                ++firstWrongOffset;
            }
        }
        ++wrongFrames;
    }
    if (overlapped == 0)
    {
        WARN("no frame was observed recording while the frame before it was still on the GPU: this run shows integrity without overlap");
    }
    INFO(std::format("{} of {} frames uploaded while the frame before was still on the GPU", overlapped, frames - 1));
    // Frame 0 names no frame: every readback matched.
    INFO(std::format("first wrong frame {}, first differing byte {}", firstWrongFrame, firstWrongOffset));
    CHECK(wrongFrames == 0);

    device.beginFrame();
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        device.destroy(outputs[frame]);
        device.destroy(readbacks[frame]);
    }
    device.destroy(busySource);
    device.destroy(busyDestination);
    device.destroy(copy);
    device.destroy(library);
    device.destroy(fence);
    device.endFrame();
    destroyDevice(device);
}
