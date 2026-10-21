//----------------------------------------------------------------------------------------------------------------------
/// @file HandoffConformanceTests.cpp
/// @brief Observes the cross-queue handoffs of chapters 09 and 10 on a GPU, each ordered by a fence.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The edge of the textures, in texels of 4 bytes, so one row is `kRowPitch` bytes once padded.
    constexpr uint32_t kEdge = 8;
    constexpr uint64_t kRowPitch = 256;
} // namespace

//======================================================================================================================
TEST_CASE("a buffer copied on the Copy queue is read on the Compute queue after a fence", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> bytes = test::pattern(4096, 3);
    const Buffer upload = kit.buffer(4096, Memory::Upload, BufferUsage::None, bytes);
    const Buffer middle = kit.buffer(4096, Memory::Gpu);
    const Buffer readback = kit.buffer(4096, Memory::Readback);
    CommandList copy = kit.list(QueueType::Copy, "test.copy");
    copy.copyBuffer(upload, 0, middle, 0, 4096);
    CommandList compute = kit.list(QueueType::Compute, "test.compute");
    compute.copyBuffer(middle, 0, readback, 0, 4096);
    // run orders the Compute list after the Copy list by the fence the Copy submission signals.
    kit.run(std::array{ copy, compute });
    CHECK(kit.read(readback) == bytes);
}

//======================================================================================================================
TEST_CASE("a new texture written on the Copy queue with no barrier is read back on the Graphics queue", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> rows = test::pattern(kRowPitch * kEdge, 7);
    const Buffer upload = kit.buffer(rows.size(), Memory::Upload, BufferUsage::None, rows);
    const Buffer readback = kit.buffer(rows.size(), Memory::Readback);
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" }
    );
    const BufferTextureLayout layout{ .offset = 0, .rowPitch = static_cast<uint32_t>(kRowPitch), .rowsPerImage = kEdge };
    const TextureRegion whole{ .extent = { kEdge, kEdge, 1 } };
    // A Copy list finds a new texture in General, so the Copy queue uploads into it with no barrier on another queue first.
    CommandList copy = kit.list(QueueType::Copy, "test.copy");
    copy.copyBufferToTexture(upload, layout, texture, whole);
    CommandList graphics = kit.list(QueueType::Graphics, "test.graphics");
    graphics.copyTextureToBuffer(texture, whole, readback, layout);
    kit.run(std::array{ copy, graphics });
    const std::vector<std::byte> got = kit.read(readback);
    for (uint32_t row = 0; row < kEdge; ++row)
    {
        CHECK(std::memcmp(got.data() + row * kRowPitch, rows.data() + row * kRowPitch, kEdge * 4) == 0);
    }
}

//======================================================================================================================
TEST_CASE("a new texture written on the Copy queue is moved from General on the Graphics queue and read back", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> rows = test::pattern(kRowPitch * kEdge, 13);
    const Buffer upload = kit.buffer(rows.size(), Memory::Upload, BufferUsage::None, rows);
    const Buffer readback = kit.buffer(rows.size(), Memory::Readback);
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" }
    );
    const BufferTextureLayout layout{ .offset = 0, .rowPitch = static_cast<uint32_t>(kRowPitch), .rowsPerImage = kEdge };
    const TextureRegion whole{ .extent = { kEdge, kEdge, 1 } };
    CommandList copy = kit.list(QueueType::Copy, "test.copy");
    copy.copyBufferToTexture(upload, layout, texture, whole);
    // The Copy queue leaves the texture in General, the layout it was created in on D3D12, so the Graphics queue's barrier
    // starts from there.
    CommandList graphics = kit.list(QueueType::Graphics, "test.graphics");
    test::transition(graphics, texture, Layout::General, Layout::CopySrc);
    graphics.copyTextureToBuffer(texture, whole, readback, layout);
    kit.run(std::array{ copy, graphics });
    const std::vector<std::byte> got = kit.read(readback);
    for (uint32_t row = 0; row < kEdge; ++row)
    {
        CHECK(std::memcmp(got.data() + row * kRowPitch, rows.data() + row * kRowPitch, kEdge * 4) == 0);
    }
}

//======================================================================================================================
TEST_CASE("fills and copies chained through the three queues by fences end with each step's bytes", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    constexpr uint64_t kHalf = 1024;
    const Buffer a = kit.buffer(2 * kHalf, Memory::Gpu, BufferUsage::ShaderWrite);
    const Buffer b = kit.buffer(2 * kHalf, Memory::Gpu, BufferUsage::ShaderWrite);
    const Buffer c = kit.buffer(2 * kHalf, Memory::Gpu, BufferUsage::ShaderWrite);
    // Compute fills A, Copy copies A's first half to B, Graphics fills B's second half and copies B to C.
    CommandList compute = kit.list(QueueType::Compute, "test.compute");
    compute.fillBuffer(a, 0, 2 * kHalf, 0x11223344u);
    CommandList copy = kit.list(QueueType::Copy, "test.copy");
    copy.copyBuffer(a, 0, b, 0, kHalf);
    CommandList graphics = kit.list(QueueType::Graphics, "test.graphics");
    graphics.fillBuffer(b, kHalf, kHalf, 0xABABABABu);
    graphics.barrier(
        std::array{ GlobalBarrier{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead } }
    );
    graphics.copyBuffer(b, 0, c, 0, 2 * kHalf);
    kit.run(std::array{ compute, copy, graphics });
    const std::vector<std::byte> got = kit.readBuffer(c);
    bool firstHalf = true;
    bool secondHalf = true;
    for (uint64_t index = 0; index < kHalf; ++index)
    {
        firstHalf = firstHalf && got[index] == std::byte{ static_cast<unsigned char>(0x44 - 0x11 * (index % 4)) };
        secondHalf = secondHalf && got[kHalf + index] == std::byte{ 0xABu };
    }
    CHECK(firstHalf);
    CHECK(secondHalf);
}

//======================================================================================================================
TEST_CASE("a wait on a fence's initial value holds nothing back", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Fence initial = *kit.device.createFence(5, "test.initial");
    const std::vector<std::byte> bytes = test::pattern(256, 11);
    const Buffer upload = kit.buffer(256, Memory::Upload, BufferUsage::None, bytes);
    const Buffer readback = kit.buffer(256, Memory::Readback);
    CommandList compute = kit.list(QueueType::Compute, "test.compute");
    compute.copyBuffer(upload, 0, readback, 0, 256);
    kit.run(std::array{ compute }, std::array{ FenceValue{ initial, 5 } });
    CHECK(kit.read(readback) == bytes);
}

//======================================================================================================================
TEST_CASE("two queues' work joined into the frame's graphics work completes before the frame retires", "[conformance]")
{
    test::requireGpu();
    constexpr uint32_t kFrames = 2;
    Device device = *createDevice(DeviceDesc{ .framesInFlight = kFrames });
    const std::vector<std::byte> bytes = test::pattern(4096, 5);
    const Buffer upload = *device.createBuffer({ .size = 4096, .memory = Memory::Upload, .label = "test.upload" }, bytes);
    const Buffer fromCopy = *device.createBuffer({ .size = 4096, .memory = Memory::Readback, .label = "test.fromCopy" });
    const Buffer fromCompute = *device.createBuffer({ .size = 4096, .memory = Memory::Readback, .label = "test.fromCompute" });
    const Fence copied = *device.createFence(0, "test.copied");
    const Fence computed = *device.createFence(0, "test.computed");
    device.beginFrame();
    CommandList copy = device.acquireCommandList(QueueType::Copy, "test.copy");
    copy.copyBuffer(upload, 0, fromCopy, 0, 4096);
    copy.end();
    CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.copyBuffer(upload, 0, fromCompute, 0, 4096);
    compute.end();
    device.queue(QueueType::Copy).submit(std::array{ copy }, {}, std::array{ FenceValue{ copied, 1 } });
    device.queue(QueueType::Compute).submit(std::array{ compute }, {}, std::array{ FenceValue{ computed, 1 } });
    // The frame's graphics work waits for both queues' values, which joins them into the frame (VU-rojo-device-0006).
    device.queue(QueueType::Graphics).submit({}, std::array{ FenceValue{ copied, 1 }, FenceValue{ computed, 1 } });
    device.endFrame();
    // After framesInFlight more frames the slot of the first has retired, with no wait of ours on a fence.
    for (uint32_t frame = 0; frame < kFrames; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    CHECK(std::memcmp(device.cpuPointer(fromCopy), bytes.data(), bytes.size()) == 0);
    CHECK(std::memcmp(device.cpuPointer(fromCompute), bytes.data(), bytes.size()) == 0);
    destroyDevice(device);
}
