//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4KernelTests.cpp
/// @brief Tests the Metal 4 backend's own kernels: the embedded library, the fill pipeline, the kernel table and fill calls.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Metal4Barriers.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Encoders.h"
#include "Metal4Kernels.h"
#include "Support/Metal4/Metal4Timing.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// The encoders of `list`.
    const metal4::Encoders& encodersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders;
    }

    //==================================================================================================================
    /// What `list` has counted so far; fails the case when a scope it recorded holds no timestamp pair, since the pinned
    /// counts include both timestamp writes.
    const core::RecordCounts& countsOf(CommandList list)
    {
        return test::timedCounts(list);
    }

    //==================================================================================================================
    /// Ends `list`, submits it to the queue of `type`, waits for it on the host and ends the frame.
    void finish(Device device, CommandList list, QueueType type = QueueType::Graphics)
    {
        list.end();
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(type).submit(std::array{ list }, {}, signal);
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
    }
} // namespace

//======================================================================================================================
TEST_CASE("createDevice loads the embedded kernel library and the fill pipeline", "[metal4]")
{
    Device first = *createDevice();
    Device second = *createDevice();
    const metal4::Kernels& kernels = DeviceImpl::of(first).kernels;
    REQUIRE(kernels.library != nullptr);
    REQUIRE(kernels.fill != nullptr);
    {
        // The label getters autorelease their strings.
        const metal4::AutoreleasePool pool;
        CHECK(metal4::toStdString(kernels.library->label()) == "rojorhi.kernels");
        CHECK(metal4::toStdString(kernels.fill->label()) == "rojorhi.kernel.fill");
    }
    // Each device loads its own library and makes its own pipelines.
    const metal4::Kernels& other = DeviceImpl::of(second).kernels;
    CHECK(other.library != nullptr);
    CHECK(other.fill != nullptr);
    CHECK(other.library != kernels.library);
    CHECK(other.fill != kernels.fill);
    destroyDevice(second);
    destroyDevice(first);
}

//======================================================================================================================
TEST_CASE("a new list creates its kernel table with its other objects", "[metal4]")
{
    Device device = *createDevice();
    device.beginFrame();
    const Counters before = device.counters();
    const CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    // The allocator, the command buffer, the list's table and its kernel table.
    CHECK(device.counters().nativeCreateCalls == before.nativeCreateCalls + 4);
    const metal4::ListState& state = CommandListImpl::of(list).state;
    REQUIRE(state.kernelTable != nullptr);
    CHECK(state.kernelTable != state.table);
    {
        const metal4::AutoreleasePool pool;
        CHECK(metal4::toStdString(state.kernelTable->label()) == "rojorhi.list.kernelTable");
    }
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an unequal-pattern fill runs the fill kernel on the open encoder and sets the list's table back", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer buffer = *device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.fill" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.copyBuffer(a, 0, buffer, 0, 256);
    REQUIRE(countsOf(list).nativeRecordCalls == 2 + 3 + 1);
    list.fillBuffer(buffer, 4, 120, 0x01020304u);
    // setComputePipelineState, setArgumentTable with the kernel table, two setAddress, dispatchThreadgroups, then
    // setArgumentTable with the list's table.
    CHECK(countsOf(list).nativeRecordCalls == 6 + 6);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(encodersOf(list).holdsWork());
    // The fill pipeline is still set on the encoder, so the next fill does not set it again.
    list.fillBuffer(buffer, 128, 64, 0x05060708u);
    CHECK(countsOf(list).nativeRecordCalls == 12 + 5);
    // A fill on a list with no open encoder opens one first, and sets the pipeline on it.
    list.beginScope("test.scope");
    const uint64_t scoped = countsOf(list).nativeRecordCalls;
    list.fillBuffer(buffer, 0, 256, 0xDEADBEEFu);
    CHECK(countsOf(list).nativeRecordCalls == scoped + 3 + 6);
    CHECK(countsOf(list).encodersOpened == 2);
    list.endScope();
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("fill parameters take arena space but count as no frame data upload", "[metal4]")
{
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.fill" });
    device.beginFrame();
    const Counters before = device.counters();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const core::CommandListCore& listCore = core::coreOf(list);
    CHECK(listCore.pageUsed == 0);
    list.fillBuffer(buffer, 0, 256, 0x11223344u);
    CHECK(listCore.pageUsed >= 8);
    const uint64_t used = listCore.pageUsed;
    list.fillBuffer(buffer, 0, 128, 0x55667788u);
    CHECK(listCore.pageUsed > used);
    CHECK(countsOf(list).frameDataUploads == 0);
    CHECK(countsOf(list).frameArenaBytes == 0);
    finish(device, list);
    const Counters after = device.counters();
    CHECK(after.frameDataUploads == before.frameDataUploads);
    CHECK(after.frameArenaBytes == before.frameArenaBytes);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a fill splits into dispatches of at most 2^31 words", "[metal4]")
{
    constexpr uint64_t kChunk = uint64_t{ 1 } << 31;
    CHECK(metal4::fillDispatchCount(1) == 1);
    CHECK(metal4::fillDispatchCount(64) == 1);
    CHECK(metal4::fillDispatchCount(kChunk - 1) == 1);
    CHECK(metal4::fillDispatchCount(kChunk) == 1);
    CHECK(metal4::fillDispatchCount(kChunk + 1) == 2);
    CHECK(metal4::fillDispatchCount(2 * kChunk) == 2);
    CHECK(metal4::fillDispatchCount(3 * kChunk + 5) == 4);
}

//======================================================================================================================
TEST_CASE("device creation time with the kernel library", "[metal4]")
{
    // Reports the median of ten creations; asserts nothing on time, which depends on the machine and its load.
    constexpr size_t kRuns = 10;
    std::array<double, kRuns> milliseconds{};
    for (double& elapsed : milliseconds)
    {
        const auto start = std::chrono::steady_clock::now();
        Device device = *createDevice();
        elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        destroyDevice(device);
    }
    std::ranges::sort(milliseconds);
    WARN(
        "createDevice: median " << (milliseconds[kRuns / 2 - 1] + milliseconds[kRuns / 2]) / 2 << " ms, min " << milliseconds.front() << " ms, max "
                                << milliseconds.back() << " ms"
    );
}

//======================================================================================================================
TEST_CASE("a kernel fill runs in the dispatch stage after an encoder-level barrier held for a dispatch", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    const Buffer c = *device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.c" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Compute, "test.list");
    list.barrier(
        std::array{ GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead | Access::CopyWrite } }
    );
    list.copyBuffer(a, 0, b, 0, 128);
    list.barrier(
        std::array{
            GlobalBarrier{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderRead } }
    );
    const metal4::BarrierState& barriers = CommandListImpl::of(list).encoders.barriers();
    REQUIRE(barriers.deferredDestination == MTL::StageDispatch);
    REQUIRE(countsOf(list).nativeBarriers == 1);
    const uint64_t before = countsOf(list).nativeRecordCalls;
    // An equal-byte fill is a blit, which the barrier does not hold back.
    list.fillBuffer(c, 0, 64, 0x77777777u);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(barriers.deferredDestination == MTL::StageDispatch);
    // The kernel fill dispatches: barrierAfterEncoderStages goes first, then the kernel's six calls.
    list.fillBuffer(c, 64, 64, 0x01020304u);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(barriers.deferredDestination == 0);
    CHECK(countsOf(list).nativeRecordCalls == before + 1 + 1 + 6);
    CHECK(countsOf(list).encodersOpened == 1);
    finish(device, list, QueueType::Compute);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a kernel pipeline is set once per open encoder and forgotten when the encoder closes", "[metal4]")
{
    Device device = *createDevice();
    const metal4::Kernels& kernels = DeviceImpl::of(device).kernels;
    const Buffer buffer = *device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.fill" });
    const Texture texture =
        *device.createTexture(TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .usage = TextureUsage::Storage, .label = "test.texture" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    // Two fills on one encoder set the pipeline once: the encoder opens (3), the first fill counts six calls, the second five.
    list.fillBuffer(buffer, 0, 64, 0x01020304u);
    list.fillBuffer(buffer, 64, 64, 0x05060708u);
    CHECK(countsOf(list).nativeRecordCalls == 2 + 3 + 6 + 5);
    CHECK(encodersOf(list).kernelPipeline() == kernels.fill);
    // A scope boundary closes the encoder, which forgets the pipeline; the next fill sets it on the new encoder.
    list.beginScope("test.scope");
    CHECK(encodersOf(list).kernelPipeline() == nullptr);
    const uint64_t scoped = countsOf(list).nativeRecordCalls;
    list.fillBuffer(buffer, 128, 64, 0x090A0B0Cu);
    CHECK(countsOf(list).nativeRecordCalls == scoped + 3 + 6);
    // Another kernel sets its own pipeline, and the fill after it sets the fill pipeline again.
    list.clearTexture(texture, {}, ClearValue{});
    CHECK(encodersOf(list).kernelPipeline() == kernels.clear[static_cast<size_t>(metal4::ClearKernel::Tex2D)]);
    const uint64_t cleared = countsOf(list).nativeRecordCalls;
    list.fillBuffer(buffer, 192, 64, 0x0D0E0F10u);
    CHECK(countsOf(list).nativeRecordCalls == cleared + 6);
    CHECK(encodersOf(list).kernelPipeline() == kernels.fill);
    list.endScope();
    CHECK(encodersOf(list).kernelPipeline() == nullptr);
    finish(device, list);
    destroyDevice(device);
}
