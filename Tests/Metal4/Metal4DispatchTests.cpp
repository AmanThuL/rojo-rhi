//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4DispatchTests.cpp
/// @brief Tests how Metal 4 records dispatches: the calls each sends and the pipeline it sets again.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/HandlePool.h"
#include "Metal4Device.h"
#include "Metal4Encoders.h"
#include "Metal4PipelineState.h"
#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The calls the lazy compute encoder makes when it opens with no barrier owed: the factory, the label and the table.
    constexpr uint64_t kOpenCalls = 3;

    //==================================================================================================================
    /// The native calls `list` has recorded so far.
    uint64_t callsOf(CommandList list)
    {
        return core::coreOf(list).counts.nativeRecordCalls;
    }

    //==================================================================================================================
    /// The encoders `list` has opened so far.
    uint64_t encodersOpenedOf(CommandList list)
    {
        return core::coreOf(list).counts.encodersOpened;
    }

    //==================================================================================================================
    /// The encoders of `list`.
    const metal4::Encoders& encodersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders;
    }

    //==================================================================================================================
    /// The payload of `pipeline` on the device of `list`.
    const metal4::PipelineState* stateOf(CommandList list, Pipeline pipeline)
    {
        return static_cast<const metal4::PipelineState*>(core::coreOf(list).device->pool<Pipeline>().native(pipeline));
    }

    //==================================================================================================================
    /// Creates a compute pipeline over the standard library's `csMain` on `device`.
    Pipeline standardCompute(Device device)
    {
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "test.library");
        return *device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" }, .label = "test.compute" });
    }

    //==================================================================================================================
    /// Ends `list`, submits it to the graphics queue, waits for it on the host and ends the frame.
    void finish(Device device, CommandList list)
    {
        list.end();
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(QueueType::Graphics).submit(std::array{ list }, {}, signal);
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
    }
} // namespace

//======================================================================================================================
TEST_CASE("a dispatch after setPipeline sends one call, after the encoder's wait on the carry fence; a second sends one more", "[metal4]")
{
    Device device = *createDevice();
    const Pipeline pipeline = standardCompute(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.setPipeline(pipeline);
    uint64_t before = callsOf(list);
    // The encoder's first dispatch on a graphics list waits on the carry fence first.
    list.dispatch(1, 1, 1);
    CHECK(callsOf(list) - before == 1 + 1);
    before = callsOf(list);
    list.dispatch(2, 1, 1);
    CHECK(callsOf(list) - before == 1);
    // A root change and a dispatch make two calls.
    before = callsOf(list);
    list.pushRoot(std::array<uint32_t, 4>{});
    list.dispatch(1, 1, 1);
    CHECK(callsOf(list) - before == 2);
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a dispatch after an unequal-pattern fill sets the caller's pipeline again, once", "[metal4]")
{
    Device device = *createDevice();
    const Pipeline pipeline = standardCompute(device);
    const Buffer buffer = *device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.buffer" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.setPipeline(pipeline);
    list.fillBuffer(buffer, 0, 256, 0x01020304u);
    REQUIRE(encodersOf(list).kernelPipeline() != stateOf(list, pipeline)->compute);
    uint64_t before = callsOf(list);
    list.dispatch(1, 1, 1);
    // The pipeline, then the dispatch.
    CHECK(callsOf(list) - before == 2);
    CHECK(encodersOf(list).kernelPipeline() == stateOf(list, pipeline)->compute);
    before = callsOf(list);
    list.dispatch(1, 1, 1);
    CHECK(callsOf(list) - before == 1);
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a dispatch on an encoder opened after a scope boundary sets the caller's pipeline again", "[metal4]")
{
    Device device = *createDevice();
    const Pipeline pipeline = standardCompute(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.setPipeline(pipeline);
    list.beginScope("test.scope");
    REQUIRE(encodersOf(list).kind() == metal4::EncoderKind::None);
    const uint64_t before = callsOf(list);
    list.dispatch(1, 1, 1);
    // The open, the wait on the carry fence, the pipeline, then the dispatch.
    CHECK(callsOf(list) - before == kOpenCalls + 1 + 1 + 1);
    CHECK(encodersOf(list).kernelPipeline() == stateOf(list, pipeline)->compute);
    list.endScope();
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a zero-group dispatch opens no encoder", "[metal4]")
{
    Device device = *createDevice();
    const Pipeline pipeline = standardCompute(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.setPipeline(pipeline);
    // The scope boundary closes the encoder the pipeline opened.
    list.beginScope("test.scope");
    REQUIRE(encodersOf(list).kind() == metal4::EncoderKind::None);
    const uint64_t calls = callsOf(list);
    const uint64_t opened = encodersOpenedOf(list);
    list.dispatch(0, 1, 1);
    list.dispatch(1, 0, 1);
    list.dispatch(1, 1, 0);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    CHECK(callsOf(list) == calls);
    CHECK(encodersOpenedOf(list) == opened);
    list.endScope();
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a compute pipeline carries the threadgroup size its library declares", "[metal4]")
{
    Device device = *createDevice();
    const std::vector<std::byte> blob = test::shaderLibraryFile("DispatchOracle");
    const ShaderLibrary library = *device.loadShaderLibrary(blob, "test.oracle");
    const Pipeline pipeline = *device.createPipeline(ComputePipelineDesc{ .compute = { library, "gridMain" }, .label = "test.grid" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const MTL::Size threads = stateOf(list, pipeline)->computeThreads;
    CHECK(threads.width == 8);
    CHECK(threads.height == 4);
    CHECK(threads.depth == 2);
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a pending barrier waits through a zero-group dispatch and is emitted once, before the next real one", "[metal4]")
{
    Device device = *createDevice();
    const Pipeline pipeline = standardCompute(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.setPipeline(pipeline);
    REQUIRE_FALSE(encodersOf(list).holdsWork());
    // The open encoder holds no work, so the barrier waits as the pending barrier.
    list.barrier(
        std::array{ GlobalBarrier{
            .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderRead } }
    );
    const core::RecordCounts& counts = core::coreOf(list).counts;
    REQUIRE(counts.nativeBarriers == 0);
    uint64_t before = callsOf(list);
    list.dispatch(0, 1, 1);
    CHECK(counts.nativeBarriers == 0);
    CHECK(callsOf(list) == before);
    // The real dispatch emits the barrier and waits on the carry fence, then dispatches.
    list.dispatch(1, 1, 1);
    CHECK(counts.nativeBarriers == 1);
    CHECK(callsOf(list) - before == 3);
    before = callsOf(list);
    list.dispatch(1, 1, 1);
    CHECK(counts.nativeBarriers == 1);
    CHECK(callsOf(list) - before == 1);
    finish(device, list);
    destroyDevice(device);
}
