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
    // Texels are 4 bytes, so a row of kEdge texels pads to kRowPitch.
    constexpr uint32_t kEdge = 8;
    constexpr uint64_t kRowPitch = 256;

    // The root of the BarrierOracle entry.
    struct DelayRoot
    {
        uint32_t view;
        uint32_t spins;
        // Always zero; it keeps the spin loop in the compiled entry.
        uint32_t mask = 0;
        uint32_t pad = 0;
        uint32_t width;
        uint32_t height;
        uint32_t pad0 = 0;
        uint32_t pad1 = 0;
        std::array<float, 4> value{};
    };
} // namespace

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

TEST_CASE("the Compute queue discards a render target the Graphics queue left in ColorAttachment and writes it", "[conformance]")
{
    test::requireGpu();
#ifndef NDEBUG
    constexpr bool kValidation = true;
#else
    constexpr bool kValidation = false;
#endif
    test::GpuKit kit(2, false, kValidation);
    const ShaderLibrary library = *kit.device.loadShaderLibrary(test::shaderLibraryFile("BarrierOracle"), "BarrierOracle");
    const Pipeline pipeline = *kit.device.createPipeline(ComputePipelineDesc{ .compute = { library, "delayedFillMain" }, .label = "test.fill" });
    const Texture texture = *kit.device.createTexture(
        TextureDesc{
            .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget | TextureUsage::Storage, .label = "test.texture" }
    );
    const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{ .usage = TextureUsage::Storage });
    const Buffer readback = kit.buffer(kRowPitch * kEdge, Memory::Readback);
    // Graphics clears the texture to red and leaves it in ColorAttachment, a layout the Compute queue does not accept.
    CommandList graphics = kit.list(QueueType::Graphics, "test.graphics");
    test::transition(graphics, texture, Layout::Undefined, Layout::ColorAttachment);
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{
        .texture = texture, .load = LoadOp::Clear, .store = StoreOp::Store, .clear = ClearColor{ 1.0f, 0.0f, 0.0f, 1.0f } } };
    graphics.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kEdge, kEdge }, .label = "test.clear" });
    graphics.endRenderPass();
    // Compute takes nothing over: its first barrier discards from Undefined (VU-rojo-sync-0025), so the layout Graphics left
    // is named nowhere on the Compute queue.
    CommandList compute = kit.list(QueueType::Compute, "test.compute");
    const std::array<TextureBarrier, 1> discard{ TextureBarrier{ .texture = texture,
                                                                 .dstStage = Stage::ComputeShader,
                                                                 .dstAccess = Access::ShaderWrite,
                                                                 .oldLayout = Layout::Undefined,
                                                                 .newLayout = Layout::ShaderWrite } };
    compute.barrier({}, {}, discard);
    compute.setPipeline(pipeline);
    compute.pushRoot(DelayRoot{ .view = static_cast<uint32_t>(view), .spins = 0, .width = kEdge, .height = kEdge, .value = { 0.0f, 1.0f, 0.0f, 1.0f } });
    compute.dispatch(1, 1, 1);
    const std::array<TextureBarrier, 1> toCopy{ TextureBarrier{ .texture = texture,
                                                                .srcStage = Stage::ComputeShader,
                                                                .srcAccess = Access::ShaderWrite,
                                                                .dstStage = Stage::Copy,
                                                                .dstAccess = Access::CopyRead,
                                                                .oldLayout = Layout::ShaderWrite,
                                                                .newLayout = Layout::CopySrc } };
    compute.barrier({}, {}, toCopy);
    const BufferTextureLayout layout{ .offset = 0, .rowPitch = static_cast<uint32_t>(kRowPitch), .rowsPerImage = kEdge };
    compute.copyTextureToBuffer(texture, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, readback, layout);
    // run orders the Compute list after the Graphics list by the fence the Graphics submission signals.
    kit.run(std::array{ graphics, compute });
    const std::vector<std::byte> got = kit.read(readback);
    const std::array<std::byte, 4> green{ std::byte{ 0 }, std::byte{ 255 }, std::byte{ 0 }, std::byte{ 255 } };
    uint32_t wrong = 0;
    for (uint32_t row = 0; row < kEdge; ++row)
    {
        for (uint32_t x = 0; x < kEdge; ++x)
        {
            wrong += std::memcmp(got.data() + row * kRowPitch + x * 4, green.data(), green.size()) == 0 ? 0 : 1;
        }
    }
    CHECK(wrong == 0u);
}

// The frame's albedo and material shape: a pair on one queue that only a fence round trip through another queue orders.
TEST_CASE("a texture a barrier moved with no destination is read on the same queue behind a fence round trip", "[conformance]")
{
    test::requireGpu();
#ifndef NDEBUG
    constexpr bool kValidation = true;
#else
    constexpr bool kValidation = false;
#endif
    // 4,096 groups of 64 threads, each spinning long enough that a read the round trip failed to hold back sees texels the
    // slow write has not stored.
    constexpr uint32_t kSlowEdge = 512;
    constexpr uint32_t kSlowSpins = 1u << 20;
    constexpr uint64_t kSlowPitch = kSlowEdge * 4;
    test::GpuKit kit(2, false, kValidation);
    const ShaderLibrary library = *kit.device.loadShaderLibrary(test::shaderLibraryFile("BarrierOracle"), "BarrierOracle");
    const Pipeline pipeline = *kit.device.createPipeline(ComputePipelineDesc{ .compute = { library, "delayedFillMain" }, .label = "test.slowFill" });
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { kSlowEdge, kSlowEdge, 1 }, .usage = TextureUsage::Storage, .label = "test.texture" }
    );
    const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{ .usage = TextureUsage::Storage });
    const Buffer readback = kit.buffer(kSlowPitch * kSlowEdge, Memory::Readback);
    const Fence fence = *kit.device.createFence(0, "test.roundTrip");
    // One fence: a setup submission signals 1, which the host waits on; Graphics then signals 2 (F1), Compute waits on it and
    // signals 3 (F2), and the later Graphics submission waits on 3 and signals 4, which the host waits on.
    const std::array<FenceValue, 1> seeded{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> f1{ FenceValue{ fence, 2 } };
    const std::array<FenceValue, 1> f2{ FenceValue{ fence, 3 } };
    const std::array<FenceValue, 1> f3{ FenceValue{ fence, 4 } };
    // Setup fills the texture with blue and leaves it in ShaderWrite. The new texture's contents are undefined, so without
    // the seed a texel a late copy read could happen to hold red; now every texel the round trip fails to order is blue.
    CommandList seed = kit.list(QueueType::Graphics, "test.seed");
    const std::array<TextureBarrier, 1> toWrite{ TextureBarrier{ .texture = texture,
                                                                 .dstStage = Stage::ComputeShader,
                                                                 .dstAccess = Access::ShaderWrite,
                                                                 .oldLayout = Layout::Undefined,
                                                                 .newLayout = Layout::ShaderWrite } };
    seed.barrier({}, {}, toWrite);
    seed.setPipeline(pipeline);
    seed.pushRoot(DelayRoot{ .view = static_cast<uint32_t>(view), .spins = 0, .width = kSlowEdge, .height = kSlowEdge, .value = { 0.0f, 0.0f, 1.0f, 1.0f } });
    seed.dispatch(kSlowEdge / 8, kSlowEdge / 8, 1);
    seed.end();
    kit.device.queue(QueueType::Graphics).submit(std::array{ seed }, {}, seeded);
    // The host has observed the seed, which orders it before the writer. The writer starts from ShaderWrite, so it records
    // no barrier from Undefined, which would discard the seed.
    REQUIRE(kit.device.wait(seeded[0], UINT64_MAX));
    // Graphics writes the texture and moves it for copies with a barrier whose destination names no stage and no access.
    CommandList writer = kit.list(QueueType::Graphics, "test.writer");
    writer.setPipeline(pipeline);
    writer.pushRoot(
        DelayRoot{ .view = static_cast<uint32_t>(view), .spins = kSlowSpins, .width = kSlowEdge, .height = kSlowEdge, .value = { 1.0f, 0.0f, 0.0f, 1.0f } }
    );
    writer.dispatch(kSlowEdge / 8, kSlowEdge / 8, 1);
    const std::array<TextureBarrier, 1> toCopy{ TextureBarrier{ .texture = texture,
                                                                .srcStage = Stage::ComputeShader,
                                                                .srcAccess = Access::ShaderWrite,
                                                                .dstStage = Stage::None,
                                                                .dstAccess = Access::None,
                                                                .oldLayout = Layout::ShaderWrite,
                                                                .newLayout = Layout::CopySrc } };
    writer.barrier({}, {}, toCopy);
    writer.end();
    // A later submission on the same queue reads it with no barrier of its own; only the round trip orders the two.
    CommandList reader = kit.list(QueueType::Graphics, "test.reader");
    reader.copyTextureToBuffer(
        texture,
        TextureRegion{ .extent = { kSlowEdge, kSlowEdge, 1 } },
        readback,
        BufferTextureLayout{ .offset = 0, .rowPitch = static_cast<uint32_t>(kSlowPitch), .rowsPerImage = kSlowEdge }
    );
    reader.end();
    kit.device.queue(QueueType::Graphics).submit(std::array{ writer }, {}, f1);
    // Compute touches nothing; it passes the value on.
    kit.device.queue(QueueType::Compute).submit({}, f1, f2);
    kit.device.queue(QueueType::Graphics).submit(std::array{ reader }, f2, f3);
    REQUIRE(kit.device.wait(f3[0], UINT64_MAX));
    const std::vector<std::byte> got = kit.read(readback);
    const std::array<std::byte, 4> red{ std::byte{ 255 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 255 } };
    uint64_t wrong = 0;
    for (uint64_t texel = 0; texel < uint64_t{ kSlowEdge } * kSlowEdge; ++texel)
    {
        wrong += std::memcmp(got.data() + texel * 4, red.data(), red.size()) == 0 ? 0 : 1;
    }
    CHECK(wrong == 0u);
}

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
