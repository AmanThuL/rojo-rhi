//----------------------------------------------------------------------------------------------------------------------
/// @file CounterTests.cpp
/// @brief Tests the counters of chapter 14 for the properties every backend shares: monotonic, folded at submission, exact where defined.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using namespace rojoRHI;
using test::PassKit;

namespace {
    //==================================================================================================================
    /// Returns true when no field of `later` is below the same field of `earlier`.
    bool monotonic(const Counters& earlier, const Counters& later)
    {
        return later.nativeCreateCalls >= earlier.nativeCreateCalls && later.nativeRecordCalls >= earlier.nativeRecordCalls &&
               later.nativeSubmitCalls >= earlier.nativeSubmitCalls && later.nativeHostCalls >= earlier.nativeHostCalls &&
               later.cpuAllocations >= earlier.cpuAllocations && later.gpuAllocations >= earlier.gpuAllocations &&
               later.frameDataUploads >= earlier.frameDataUploads && later.frameArenaBytes >= earlier.frameArenaBytes &&
               later.frameArenaGrowths >= earlier.frameArenaGrowths && later.descriptorWrites >= earlier.descriptorWrites &&
               later.globalBarriers >= earlier.globalBarriers && later.bufferBarriers >= earlier.bufferBarriers &&
               later.textureBarriers >= earlier.textureBarriers && later.nativeBarriers >= earlier.nativeBarriers &&
               later.pipelinesCreated >= earlier.pipelinesCreated && later.pipelineCacheHits >= earlier.pipelineCacheHits &&
               later.pipelineCacheMisses >= earlier.pipelineCacheMisses && later.pipelineCompileNanoseconds >= earlier.pipelineCompileNanoseconds &&
               later.encodersOpened >= earlier.encodersOpened && later.deferredDestructionsRetired >= earlier.deferredDestructionsRetired;
    }

    //==================================================================================================================
    /// Ends the kit's list, submits it and ends the frame, leaving the device alive.
    void submit(PassKit& kit)
    {
        kit.list.end();
        const std::array<CommandList, 1> lists{ kit.list };
        kit.device.queue(kit.type).submit(lists);
        kit.device.endFrame();
    }
} // namespace

//======================================================================================================================
TEST_CASE("counters never decrease across creation, recording, submission and retirement", "[contract]")
{
    PassKit kit;
    const Counters start = kit.device.counters();
    kit.readyToDraw();
    kit.list.draw(3, 1, 0, 0);
    const Counters recorded = kit.device.counters();
    CHECK(monotonic(start, recorded));
    kit.list.endRenderPass();
    submit(kit);
    const Counters submitted = kit.device.counters();
    CHECK(monotonic(recorded, submitted));
    kit.device.destroy(kit.library);
    kit.device.beginFrame();
    kit.device.endFrame();
    kit.device.beginFrame();
    kit.device.endFrame();
    CHECK(monotonic(submitted, kit.device.counters()));
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("recording counts reach the device only when the list is submitted", "[contract]")
{
    PassKit kit;
    const Counters before = kit.device.counters();
    kit.list.beginRenderPass(test::fullPass(std::array<ColorAttachment, 1>{ ColorAttachment{ .texture = kit.target() } }));
    kit.list.endRenderPass();
    const std::array<std::byte, 16> bytes{};
    (void)kit.list.uploadFrameData(bytes, 16);
    kit.list.barrier(std::array<GlobalBarrier, 1>{ GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::All, Access::ShaderRead } });
    const Counters recorded = kit.device.counters();
    CHECK(recorded.nativeRecordCalls == before.nativeRecordCalls);
    CHECK(recorded.frameDataUploads == before.frameDataUploads);
    CHECK(recorded.globalBarriers == before.globalBarriers);
    CHECK(recorded.encodersOpened == before.encodersOpened);
    submit(kit);
    const Counters submitted = kit.device.counters();
    CHECK(submitted.nativeRecordCalls > before.nativeRecordCalls);
    CHECK(submitted.frameDataUploads == before.frameDataUploads + 1);
    CHECK(submitted.globalBarriers == before.globalBarriers + 1);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("uploadFrameData counts one upload and the bytes it copies, not the padding", "[contract]")
{
    PassKit kit;
    const Counters before = kit.device.counters();
    const std::array<std::byte, 100> first{};
    const std::array<std::byte, 28> second{};
    (void)kit.list.uploadFrameData(first, 256);
    (void)kit.list.uploadFrameData(second, 256);
    submit(kit);
    const Counters after = kit.device.counters();
    CHECK(after.frameDataUploads == before.frameDataUploads + 2);
    CHECK(after.frameArenaBytes == before.frameArenaBytes + 128);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a debug scope claims arena space but is no frame data upload", "[contract]")
{
    PassKit kit;
    const Counters before = kit.device.counters();
    kit.list.beginScope("counted.scope");
    kit.list.endScope();
    submit(kit);
    const Counters after = kit.device.counters();
    CHECK(after.frameDataUploads == before.frameDataUploads);
    CHECK(after.frameArenaBytes == before.frameArenaBytes);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("barrier records are counted by kind", "[contract]")
{
    PassKit kit;
    const Buffer buffer = kit.buffer(BufferUsage::None, 256);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 3);
    const std::array<GlobalBarrier, 2> globals{ GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::All, Access::ShaderRead },
                                                GlobalBarrier{ Stage::All, Access::None, Stage::All, Access::None } };
    const std::array<BufferBarrier, 1> buffers{ BufferBarrier{ .buffer = buffer, .srcStage = Stage::Copy, .dstStage = Stage::All } };
    const std::array<TextureBarrier, 3> textures{
        TextureBarrier{ .texture = texture, .range = { .baseMip = 0, .mipCount = 1 }, .dstStage = Stage::All, .newLayout = Layout::ShaderRead },
        TextureBarrier{ .texture = texture, .range = { .baseMip = 1, .mipCount = 1 }, .dstStage = Stage::All, .newLayout = Layout::ShaderRead },
        TextureBarrier{ .texture = texture, .range = { .baseMip = 2, .mipCount = 1 }, .dstStage = Stage::All, .newLayout = Layout::ShaderRead },
    };
    const Counters before = kit.device.counters();
    kit.list.barrier(globals, buffers, textures);
    submit(kit);
    const Counters after = kit.device.counters();
    CHECK(after.globalBarriers == before.globalBarriers + 2);
    CHECK(after.bufferBarriers == before.bufferBarriers + 1);
    CHECK(after.textureBarriers == before.textureBarriers + 3);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("every pipeline creation is a cache hit or a miss", "[contract]")
{
    PassKit kit;
    const Counters before = kit.device.counters();
    (void)kit.compute();
    const Counters after = kit.device.counters();
    CHECK(after.pipelinesCreated == before.pipelinesCreated + 1);
    CHECK(after.pipelineCacheHits + after.pipelineCacheMisses == before.pipelineCacheHits + before.pipelineCacheMisses + 1);
    CHECK(after.pipelineCompileNanoseconds >= before.pipelineCompileNanoseconds);
    submit(kit);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("creating a buffer counts a native creation and a native memory object", "[contract]")
{
    PassKit kit;
    const Counters before = kit.device.counters();
    (void)kit.buffer(BufferUsage::None, 256);
    const Counters after = kit.device.counters();
    CHECK(after.nativeCreateCalls > before.nativeCreateCalls);
    CHECK(after.gpuAllocations > before.gpuAllocations);
    submit(kit);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("each heap-slot write counts one descriptor write", "[contract]")
{
    PassKit kit;
    const Texture texture = kit.texture();
    const Counters before = kit.device.counters();
    const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{});
    const SamplerIndex sampler = *kit.device.createSampler(SamplerDesc{});
    const IndexRange views = *kit.device.reserveViews(1);
    kit.device.writeView(views, 0, texture, TextureViewDesc{});
    const IndexRange samplers = *kit.device.reserveSamplers(1);
    kit.device.writeSampler(samplers, 0, SamplerDesc{});
    CHECK(kit.device.counters().descriptorWrites == before.descriptorWrites + 4);
    kit.device.destroy(view);
    kit.device.destroy(sampler);
    kit.device.release(views);
    kit.device.release(samplers);
    submit(kit);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a submission and a fence query count native queue and host calls", "[contract]")
{
    PassKit kit;
    const Fence fence = *kit.device.createFence(0, "counter.fence");
    const Counters before = kit.device.counters();
    kit.list.end();
    const std::array<CommandList, 1> lists{ kit.list };
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    kit.device.queue(kit.type).submit(lists, {}, signal);
    const Counters submitted = kit.device.counters();
    CHECK(submitted.nativeSubmitCalls > before.nativeSubmitCalls);
    (void)kit.device.completedValue(fence);
    CHECK(kit.device.counters().nativeHostCalls > before.nativeHostCalls);
    kit.device.endFrame();
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("the counts of a list that is not yet submitted do not appear in the device", "[contract]")
{
    PassKit kit;
    CommandList held = kit.list;
    CommandList other = kit.device.acquireCommandList(kit.type, "counter.other");
    const std::array<std::byte, 16> small{};
    const std::array<std::byte, 32> large{};
    (void)held.uploadFrameData(small, 16);
    held.barrier(std::array<GlobalBarrier, 1>{ GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::All, Access::ShaderRead } });
    (void)other.uploadFrameData(large, 16);
    held.end();
    other.end();
    const Counters before = kit.device.counters();
    const std::array<CommandList, 1> second{ other };
    kit.device.queue(kit.type).submit(second);
    const Counters afterOther = kit.device.counters();
    CHECK(afterOther.frameDataUploads == before.frameDataUploads + 1);
    CHECK(afterOther.frameArenaBytes == before.frameArenaBytes + 32);
    CHECK(afterOther.globalBarriers == before.globalBarriers);
    const std::array<CommandList, 1> first{ held };
    kit.device.queue(kit.type).submit(first);
    const Counters afterHeld = kit.device.counters();
    CHECK(afterHeld.frameArenaBytes == before.frameArenaBytes + 48);
    CHECK(afterHeld.globalBarriers == before.globalBarriers + 1);
    kit.device.endFrame();
    destroyDevice(kit.device);
}
