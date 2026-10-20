//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4BarrierTests.cpp
/// @brief Tests how Metal 4 lowers barrier batches: the stage table, the encoder-level and queue-level forms, the trailing barrier and visibility.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Metal4Barriers.h"
#include "Metal4Device.h"
#include "Metal4Encoders.h"
#include "Support/Metal4/Metal4Timing.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace rojoRHI;

namespace {
    /// Every stage a copy names, on either side.
    constexpr MTL::Stages kCopyStages = MTL::StageBlit | MTL::StageDispatch | MTL::StageFragment;

    //==================================================================================================================
    /// The encoders of `list`.
    const metal4::Encoders& encodersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders;
    }

    //==================================================================================================================
    /// The barrier state of `list`.
    const metal4::BarrierState& barriersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders.barriers();
    }

    //==================================================================================================================
    /// What `list` has counted so far; fails the case when a scope it recorded holds no timestamp pair, since the pinned
    /// counts include both timestamp writes.
    const core::RecordCounts& countsOf(CommandList list)
    {
        return test::timedCounts(list);
    }

    //==================================================================================================================
    /// A global barrier from copy writes to copy reads and writes.
    GlobalBarrier copyToCopy()
    {
        return GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead | Access::CopyWrite
        };
    }

    //==================================================================================================================
    /// Submits the ended `lists` to the queue of `type`, waits for them on the host and ends the frame.
    void submitAndWait(Device device, std::span<const CommandList> lists, QueueType type = QueueType::Graphics)
    {
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(type).submit(lists, {}, signal);
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
    }
} // namespace

//======================================================================================================================
TEST_CASE("every Stage bit lowers to chapter 09's Metal stages", "[metal4]")
{
    CHECK(metal4::metalStages(Stage::None) == 0);
    CHECK(metal4::metalStages(Stage::IndirectArgs) == (MTL::StageDispatch | MTL::StageVertex | MTL::StageObject | MTL::StageMesh));
    CHECK(metal4::metalStages(Stage::IndexInput) == MTL::StageVertex);
    CHECK(metal4::metalStages(Stage::VertexShader) == MTL::StageVertex);
    CHECK(metal4::metalStages(Stage::TaskShader) == MTL::StageObject);
    CHECK(metal4::metalStages(Stage::MeshShader) == MTL::StageMesh);
    CHECK(metal4::metalStages(Stage::FragmentShader) == MTL::StageFragment);
    CHECK(metal4::metalStages(Stage::ComputeShader) == MTL::StageDispatch);
    CHECK(metal4::metalStages(Stage::ColorAttachment) == MTL::StageFragment);
    CHECK(metal4::metalStages(Stage::DepthStencil) == MTL::StageFragment);
    CHECK(metal4::metalStages(Stage::Copy) == kCopyStages);
    CHECK(metal4::metalStages(Stage::Resolve) == MTL::StageFragment);
    CHECK(metal4::metalStages(Stage::AllGraphics) == (MTL::StageDispatch | MTL::StageVertex | MTL::StageObject | MTL::StageMesh | MTL::StageFragment));
    CHECK(metal4::metalStages(Stage::All) == MTL::StageAll);
    // A set lowers to the union of its bits.
    CHECK(metal4::metalStages(Stage::ComputeShader | Stage::VertexShader) == (MTL::StageDispatch | MTL::StageVertex));
    CHECK(metal4::metalStages(Stage::TaskShader | Stage::Copy) == (MTL::StageObject | kCopyStages));
}

//======================================================================================================================
TEST_CASE("the compute encoder runs its copies in a stage the lowering gives it", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.copyBuffer(a, 0, b, 0, 256);
    MTL4::ComputeCommandEncoder* encoder = encodersOf(list).computeEncoder();
    REQUIRE(encoder != nullptr);
    // stages() names the stages of the commands the encoder holds so far: a copy's is the blit stage.
    CHECK(encoder->stages() == MTL::StageBlit);
    CHECK((encoder->stages() & ~metal4::kComputeStages) == 0);
    CHECK(encodersOf(list).stages() == metal4::kComputeStages);
    list.end();
    submitAndWait(device, std::array{ list });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a barrier at the start of a list is emitted by the first encoder at queue level", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    // A Copy list runs blits only, so its barriers name the blit stage alone.
    CommandList list = device.acquireCommandList(QueueType::Copy, "test.list");
    const uint64_t label = test::encoderLabel(list);
    list.barrier(std::array{ copyToCopy() });
    // Nothing is emitted yet: no encoder is open, so the barrier waits for one.
    CHECK(countsOf(list).nativeBarriers == 0);
    CHECK(countsOf(list).nativeRecordCalls == 2);
    CHECK(countsOf(list).globalBarriers == 1);
    CHECK(barriersOf(list).pendingSource == MTL::StageBlit);
    CHECK(barriersOf(list).pendingDestination == MTL::StageBlit);
    CHECK(barriersOf(list).pendingVisibility == MTL4::VisibilityOptionDevice);
    list.copyBuffer(a, 0, b, 0, 256);
    // The opening adds computeCommandEncoder, setLabel, setArgumentTable and barrierAfterQueueStages; then the copy.
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 2 + 3 + label + 1);
    CHECK(barriersOf(list).pendingDestination == 0);
    CHECK(barriersOf(list).pendingSource == 0);
    CHECK(barriersOf(list).pendingVisibility == 0);
    CHECK(barriersOf(list).openSource == MTL::StageBlit);
    CHECK(barriersOf(list).openDestination == MTL::StageBlit);
    list.end();
    // endEncoding and endCommandBuffer; nothing is pending, so no trailing encoder.
    CHECK(countsOf(list).nativeRecordCalls == 6 + label + 2);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(barriersOf(list).openSource == 0);
    CHECK(barriersOf(list).openDestination == 0);
    submitAndWait(device, std::array{ list }, QueueType::Copy);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a barrier whose sources the opening barrier covered stays in the open encoder", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Copy, "test.list");
    const uint64_t label = test::encoderLabel(list);
    list.barrier(std::array{ copyToCopy() });
    list.copyBuffer(a, 0, b, 0, 128);
    CHECK(countsOf(list).nativeRecordCalls == 6 + label);
    list.barrier(std::array{ copyToCopy() });
    // The encoder stays open and holds the barrier back until a command follows it in the encoder.
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(encodersOf(list).holdsWork());
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 6 + label);
    CHECK(barriersOf(list).pendingDestination == 0);
    CHECK(barriersOf(list).deferredDestination == MTL::StageBlit);
    // A second barrier with no command between them merges with the first.
    list.barrier(std::array{ copyToCopy() });
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 6 + label);
    // barrierAfterEncoderStages goes before the copy that follows it.
    list.copyBuffer(b, 0, a, 0, 128);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(countsOf(list).nativeRecordCalls == 6 + label + 1 + 1);
    CHECK(barriersOf(list).deferredDestination == 0);
    list.end();
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(countsOf(list).nativeRecordCalls == 8 + label + 2);
    submitAndWait(device, std::array{ list }, QueueType::Copy);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an encoder-level barrier with no command after it is carried at end by its encoder", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Copy, "test.list");
    const uint64_t label = test::encoderLabel(list);
    list.barrier(std::array{ copyToCopy() });
    list.copyBuffer(a, 0, b, 0, 128);
    list.barrier(std::array{ copyToCopy() });
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 6 + label);
    list.end();
    // The held barrier joins the pending one, whose sources the encoder's opening barrier waited for: the encoder emits
    // barrierAfterStages after the stage it ran, then endEncoding; then endCommandBuffer. No trailing encoder.
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 1 + 1);
    CHECK(countsOf(list).nativeRecordCalls == 6 + label + 1 + 1 + 1);
    CHECK(barriersOf(list).deferredDestination == 0);
    CHECK(barriersOf(list).pendingDestination == 0);
    submitAndWait(device, std::array{ list }, QueueType::Copy);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an encoder-level barrier closed by a scope boundary is emitted by the next encoder at queue level", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Copy, "test.list");
    list.barrier(std::array{ copyToCopy() });
    list.copyBuffer(a, 0, b, 0, 128);
    list.barrier(std::array{ copyToCopy() });
    list.beginScope("test.scope");
    // The encoder ended with the barrier held back, so the barrier waits for the next encoder at queue level.
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(barriersOf(list).deferredDestination == 0);
    CHECK(barriersOf(list).pendingSource == MTL::StageBlit);
    CHECK(barriersOf(list).pendingDestination == MTL::StageBlit);
    list.copyBuffer(b, 0, a, 0, 128);
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(barriersOf(list).pendingDestination == 0);
    CHECK(barriersOf(list).openDestination == MTL::StageBlit);
    list.endScope();
    list.end();
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeBarriers == 2);
    submitAndWait(device, std::array{ list }, QueueType::Copy);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a barrier after uncovered work closes the encoder and the next encoder emits it", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Copy, "test.list");
    const uint64_t label = test::encoderLabel(list);
    list.copyBuffer(a, 0, b, 0, 128);
    // The encoder opened with no barrier, so its opening covered nothing.
    CHECK(barriersOf(list).openSource == 0);
    CHECK(barriersOf(list).openDestination == 0);
    CHECK(countsOf(list).nativeRecordCalls == 5 + label);
    list.barrier(std::array{ copyToCopy() });
    // The copy's sources may include earlier lists' work, so the barrier is queue-level: the encoder ends (endEncoding).
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    CHECK(countsOf(list).nativeBarriers == 0);
    CHECK(countsOf(list).nativeRecordCalls == 5 + label + 1);
    CHECK(barriersOf(list).pendingSource == MTL::StageBlit);
    CHECK(barriersOf(list).pendingDestination == MTL::StageBlit);
    list.copyBuffer(b, 0, a, 0, 128);
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 6 + label + 3 + label + 1);
    CHECK(barriersOf(list).pendingDestination == 0);
    list.end();
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeRecordCalls == 10 + 2 * label + 2);
    submitAndWait(device, std::array{ list }, QueueType::Copy);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a graphics list owes the fragment part of a Copy barrier and emits it at end on a closing encoder", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    list.barrier(std::array{ copyToCopy() });
    CHECK(barriersOf(list).pendingSource == kCopyStages);
    CHECK(barriersOf(list).pendingDestination == kCopyStages);
    list.copyBuffer(a, 0, b, 0, 128);
    // The compute encoder emits the dispatch and blit part; the fragment part stays owed.
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 6 + label);
    CHECK(barriersOf(list).pendingSource == kCopyStages);
    CHECK(barriersOf(list).pendingDestination == MTL::StageFragment);
    CHECK(barriersOf(list).openSource == kCopyStages);
    CHECK(barriersOf(list).openDestination == metal4::kComputeStages);
    // Its sources are covered, so the next barrier stays in the encoder, and its fragment part joins what is owed.
    list.barrier(std::array{ copyToCopy() });
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 6 + label);
    CHECK(barriersOf(list).pendingDestination == MTL::StageFragment);
    list.copyBuffer(b, 0, a, 0, 128);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(countsOf(list).nativeRecordCalls == 8 + label);
    list.end();
    // The encoder's opening barrier waited for every pending source, so the encoder carries what is owed: updateFence for
    // the dispatch stage it gave back, barrierAfterStages for the fragment stage, then endEncoding; then endCommandBuffer.
    // No memory aliases, so the global barriers carry no alias visibility that would keep the dispatch stage in the barrier.
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 3);
    CHECK(countsOf(list).nativeRecordCalls == 8 + label + 2 + 1 + 1);
    CHECK(barriersOf(list).pendingSource == 0);
    CHECK(barriersOf(list).pendingDestination == 0);
    CHECK(barriersOf(list).pendingVisibility == 0);
    submitAndWait(device, std::array{ list });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Compute and Copy lists owe no render stage", "[metal4]")
{
    const QueueType type = GENERATE(QueueType::Compute, QueueType::Copy);
    CAPTURE(static_cast<int>(type));
    // A Compute list runs the compute encoder's stages; a Copy list runs blits only.
    const MTL::Stages stages = type == QueueType::Compute ? metal4::kComputeStages : MTL::Stages{ MTL::StageBlit };
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(type, "test.list");
    const uint64_t label = test::encoderLabel(list);
    // All stands for the whole queue.
    list.barrier(std::array{ GlobalBarrier{ .srcStage = Stage::All, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead } });
    CHECK(barriersOf(list).pendingSource == stages);
    CHECK(barriersOf(list).pendingDestination == stages);
    list.copyBuffer(a, 0, b, 0, 128);
    CHECK(barriersOf(list).pendingDestination == 0);
    CHECK(countsOf(list).nativeBarriers == 1);
    list.end();
    if (type == QueueType::Copy)
    {
        // The encoder ran a command in every stage its opening barrier held: nothing is owed.
        CHECK(countsOf(list).encodersOpened == 1);
        CHECK(countsOf(list).nativeBarriers == 1);
        CHECK(countsOf(list).nativeRecordCalls == 2 + 3 + label + 1 + 2);
    }
    else
    {
        // The encoder ran no dispatch, so the dispatch stage is still owed, and the encoder carries it with a barrier.
        CHECK(countsOf(list).encodersOpened == 1);
        CHECK(countsOf(list).nativeBarriers == 2);
        CHECK(countsOf(list).nativeRecordCalls == 2 + 3 + label + 1 + 1 + 1 + 1);
    }
    submitAndWait(device, std::array{ list }, type);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Stage::All on a graphics list names only the stages of compute and render encoders", "[metal4]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.barrier(std::array{ GlobalBarrier{ .srcStage = Stage::All, .dstStage = Stage::All } });
    CHECK(barriersOf(list).pendingSource == (metal4::kComputeStages | metal4::kRenderStages));
    CHECK(barriersOf(list).pendingDestination == (metal4::kComputeStages | metal4::kRenderStages));
    list.end();
    submitAndWait(device, std::array{ list });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an encoder that ran no command in a stage its opening barrier held returns that stage to the pending barrier", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Compute, "test.list");
    list.barrier(std::array{ copyToCopy() });
    list.copyBuffer(a, 0, b, 0, 128);
    CHECK(barriersOf(list).openDestination == metal4::kComputeStages);
    CHECK(barriersOf(list).pendingDestination == 0);
    // The encoder ran only a blit: a dispatch in a later encoder is not ordered by its opening barrier.
    list.beginScope("test.scope");
    CHECK(barriersOf(list).pendingSource == metal4::kComputeStages);
    CHECK(barriersOf(list).pendingDestination == MTL::StageDispatch);
    CHECK(barriersOf(list).pendingVisibility == MTL4::VisibilityOptionDevice);
    list.copyBuffer(b, 0, a, 0, 128);
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(barriersOf(list).openDestination == MTL::StageDispatch);
    list.endScope();
    CHECK(barriersOf(list).pendingDestination == MTL::StageDispatch);
    list.end();
    CHECK(countsOf(list).encodersOpened == 3);
    CHECK(countsOf(list).nativeBarriers == 4);
    CHECK(barriersOf(list).pendingDestination == 0);
    submitAndWait(device, std::array{ list }, QueueType::Compute);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an encoder-level barrier waits for a command in its destination stages", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Compute, "test.list");
    list.barrier(std::array{ copyToCopy() });
    list.copyBuffer(a, 0, b, 0, 128);
    list.barrier(
        std::array{
            GlobalBarrier{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderRead } }
    );
    CHECK(barriersOf(list).deferredDestination == MTL::StageDispatch);
    // A copy is a blit, which the barrier does not hold back: it stays held for a dispatch.
    list.copyBuffer(b, 0, a, 0, 128);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(barriersOf(list).deferredDestination == MTL::StageDispatch);
    list.end();
    // No dispatch followed, so the barrier and the unrun dispatch stage are owed, and the encoder carries them.
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(barriersOf(list).deferredDestination == 0);
    submitAndWait(device, std::array{ list }, QueueType::Compute);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a batch with a None source records no native barrier, and an empty batch records nothing", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    list.barrier({});
    CHECK(countsOf(list).globalBarriers == 0);
    CHECK(countsOf(list).nativeRecordCalls == 2);
    list.copyBuffer(a, 0, b, 0, 128);
    CHECK(countsOf(list).nativeRecordCalls == 5 + label);
    // No earlier work to wait for: nothing to order, and the encoder stays open.
    list.barrier(std::array{ GlobalBarrier{ .srcStage = Stage::None, .dstStage = Stage::Copy } });
    CHECK(countsOf(list).globalBarriers == 1);
    // No later work to hold back: nothing to order either.
    list.barrier({}, std::array{ BufferBarrier{ .buffer = b, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::None } });
    CHECK(countsOf(list).bufferBarriers == 1);
    list.barrier({}, {}, {});
    CHECK(countsOf(list).nativeBarriers == 0);
    CHECK(countsOf(list).nativeRecordCalls == 5 + label);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(encodersOf(list).holdsWork());
    CHECK(barriersOf(list).pendingDestination == 0);
    CHECK(barriersOf(list).pendingSource == 0);
    list.end();
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeRecordCalls == 5 + label + 2);
    submitAndWait(device, std::array{ list });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("visibility is Device for named accesses and none for an execution-only batch", "[metal4]")
{
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer({ .size = 256, .label = "test.buffer" });
    const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" };
    const Texture dedicated = *device.createTexture(desc);
    const BufferBarrier copyRead{
        .buffer = buffer, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead
    };
    const BufferBarrier executionOnly{ .buffer = buffer, .srcStage = Stage::Copy, .dstStage = Stage::Copy };
    const GlobalBarrier globalOnly{ .srcStage = Stage::Copy, .dstStage = Stage::Copy };
    const TextureBarrier discard{ .texture = dedicated,
                                  .srcStage = Stage::Copy,
                                  .dstStage = Stage::Copy,
                                  .dstAccess = Access::CopyWrite,
                                  .oldLayout = Layout::Undefined,
                                  .newLayout = Layout::CopyDst };
    device.beginFrame();
    std::vector<CommandList> lists;
    const auto visibilityOf = [&](std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers, std::span<const TextureBarrier> textures) {
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.barrier(globals, buffers, textures);
        lists.push_back(list);
        return barriersOf(list).pendingVisibility;
    };
    CHECK(visibilityOf({}, std::array{ copyRead }, {}) == MTL4::VisibilityOptionDevice);
    CHECK(visibilityOf({}, std::array{ executionOnly }, {}) == MTL4::VisibilityOptionNone);
    CHECK(visibilityOf(std::array{ globalOnly }, {}, {}) == MTL4::VisibilityOptionNone);
    CHECK(visibilityOf(std::array{ copyToCopy() }, {}, {}) == MTL4::VisibilityOptionDevice);
    CHECK(visibilityOf({}, {}, std::array{ discard }) == MTL4::VisibilityOptionDevice);
    for (CommandList list : lists)
    {
        list.end();
    }
    submitAndWait(device, lists);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a discard of a Sparse texture carries alias visibility", "[metal4]")
{
    Device device = *createDevice();
    const TextureDesc plainDesc{ .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::Sampled, .label = "test.plain" };
    const Texture plain = *device.createTexture(plainDesc);
    const auto discardOf = [](Texture texture) {
        return TextureBarrier{ .texture = texture,
                               .srcStage = Stage::Copy,
                               .dstStage = Stage::Copy,
                               .dstAccess = Access::CopyWrite,
                               .oldLayout = Layout::Undefined,
                               .newLayout = Layout::CopyDst };
    };
    device.beginFrame();
    std::vector<CommandList> lists;
    const auto visibilityOf = [&](Texture texture) {
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.barrier({}, {}, std::array{ discardOf(texture) });
        lists.push_back(list);
        return barriersOf(list).pendingVisibility;
    };
    CHECK((visibilityOf(plain) & MTL4::VisibilityOptionResourceAlias) == 0);
    const TextureDesc sparseDesc{
        .format = Format::RGBA8Unorm, .extent = { 256, 256, 1 }, .usage = TextureUsage::Sampled | TextureUsage::Sparse, .label = "test.sparse"
    };
    const Texture sparse = *device.createTexture(sparseDesc);
    CHECK((visibilityOf(sparse) & MTL4::VisibilityOptionResourceAlias) != 0);
    for (CommandList list : lists)
    {
        list.end();
    }
    submitAndWait(device, lists);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a global barrier adds ResourceAlias only while placed memory aliases", "[metal4]")
{
    Device device = *createDevice();
    const BufferDesc desc{ .size = 256, .label = "test.placed" };
    const PlacementRequirements requirements = device.placementRequirements(desc);
    const Heap heap = *device.createHeap({ .size = requirements.size * 4, .label = "test.heap" });
    device.beginFrame();
    std::vector<CommandList> lists;
    const auto visibility = [&] {
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.barrier(std::array{ copyToCopy() });
        lists.push_back(list);
        return barriersOf(list).pendingVisibility;
    };
    const MTL4::VisibilityOptions plain = MTL4::VisibilityOptionDevice;
    const MTL4::VisibilityOptions aliasing = MTL4::VisibilityOptionDevice | MTL4::VisibilityOptionResourceAlias;
    CHECK(visibility() == plain);
    const Buffer a = *device.createBuffer(desc, {}, { heap, 0 });
    const Buffer apart = *device.createBuffer(desc, {}, { heap, requirements.size });
    CHECK(visibility() == plain);
    const Buffer b = *device.createBuffer(desc, {}, { heap, 0 });
    CHECK(visibility() == aliasing);
    device.destroy(b);
    // The resource that stays was overlapped while it lived, and its first use may follow a resource that held its memory.
    CHECK(visibility() == aliasing);
    device.destroy(a);
    CHECK(visibility() == plain);
    // A buffer placed where a destroyed one was takes its memory over.
    const Buffer c = *device.createBuffer(desc, {}, { heap, 0 });
    CHECK(visibility() == aliasing);
    device.destroy(c);
    device.destroy(apart);
    CHECK(visibility() == plain);
    for (CommandList list : lists)
    {
        list.end();
    }
    submitAndWait(device, lists);
    device.destroy(heap);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a resource placed where a destroyed one was is aliased with no other live resource", "[metal4]")
{
    Device device = *createDevice();
    const BufferDesc bufferDesc{ .size = 256, .label = "test.placed" };
    const TextureDesc textureDesc{ .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" };
    const Heap bufferHeap = *device.createHeap({ .size = device.placementRequirements(bufferDesc).size, .label = "test.buffers" });
    const Heap textureHeap = *device.createHeap({ .size = device.placementRequirements(textureDesc).size, .label = "test.textures" });
    const Buffer oldBuffer = *device.createBuffer(bufferDesc, {}, { bufferHeap, 0 });
    const Texture oldTexture = *device.createTexture(textureDesc, {}, { textureHeap, 0 });
    device.destroy(oldBuffer);
    device.destroy(oldTexture);
    const Buffer buffer = *device.createBuffer(bufferDesc, {}, { bufferHeap, 0 });
    const Texture texture = *device.createTexture(textureDesc, {}, { textureHeap, 0 });
    device.beginFrame();
    CommandList globalList = device.acquireCommandList(QueueType::Graphics, "test.global");
    globalList.barrier(std::array{ copyToCopy() });
    CHECK(barriersOf(globalList).pendingVisibility == (MTL4::VisibilityOptionDevice | MTL4::VisibilityOptionResourceAlias));
    CommandList textureList = device.acquireCommandList(QueueType::Graphics, "test.texture");
    textureList.barrier(
        {},
        {},
        std::array{ TextureBarrier{ .texture = texture,
                                    .srcStage = Stage::Copy,
                                    .dstStage = Stage::Copy,
                                    .dstAccess = Access::CopyWrite,
                                    .oldLayout = Layout::Undefined,
                                    .newLayout = Layout::CopyDst } }
    );
    CHECK(barriersOf(textureList).pendingVisibility == (MTL4::VisibilityOptionDevice | MTL4::VisibilityOptionResourceAlias));
    globalList.end();
    textureList.end();
    submitAndWait(device, std::array{ globalList, textureList });
    device.destroy(buffer);
    device.destroy(texture);
    device.destroy(bufferHeap);
    device.destroy(textureHeap);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a global barrier adds ResourceAlias while a sparse buffer lives", "[metal4]")
{
    Device device = *createDevice();
    if (!device.capabilities().placementSparse)
    {
        destroyDevice(device);
        SKIP("the device reports no sparse resources");
    }
    device.beginFrame();
    std::vector<CommandList> lists;
    const auto visibility = [&] {
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.barrier(std::array{ copyToCopy() });
        lists.push_back(list);
        return barriersOf(list).pendingVisibility;
    };
    CHECK(visibility() == MTL4::VisibilityOptionDevice);
    const Buffer sparse = *device.createBuffer({ .size = 1 << 17, .usage = BufferUsage::Sparse, .label = "test.sparse" });
    CHECK(visibility() == (MTL4::VisibilityOptionDevice | MTL4::VisibilityOptionResourceAlias));
    device.destroy(sparse);
    CHECK(visibility() == MTL4::VisibilityOptionDevice);
    for (CommandList list : lists)
    {
        list.end();
    }
    submitAndWait(device, lists);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture barrier from Undefined adds ResourceAlias only for a placed texture whose memory aliases", "[metal4]")
{
    Device device = *createDevice();
    const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" };
    const PlacementRequirements requirements = device.placementRequirements(desc);
    const Heap heap = *device.createHeap({ .size = requirements.size * 4, .label = "test.heap" });
    const Texture first = *device.createTexture(desc, {}, { heap, 0 });
    const Texture apart = *device.createTexture(desc, {}, { heap, requirements.size });
    device.beginFrame();
    std::vector<CommandList> lists;
    const auto visibility = [&](Texture texture, Layout oldLayout = Layout::Undefined) {
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.barrier(
            {},
            {},
            std::array{ TextureBarrier{ .texture = texture,
                                        .srcStage = Stage::Copy,
                                        .srcAccess = oldLayout == Layout::Undefined ? Access::None : Access::CopyRead,
                                        .dstStage = Stage::Copy,
                                        .dstAccess = Access::CopyWrite,
                                        .oldLayout = oldLayout,
                                        .newLayout = Layout::CopyDst } }
        );
        lists.push_back(list);
        return barriersOf(list).pendingVisibility;
    };
    const MTL4::VisibilityOptions plain = MTL4::VisibilityOptionDevice;
    const MTL4::VisibilityOptions aliasing = MTL4::VisibilityOptionDevice | MTL4::VisibilityOptionResourceAlias;
    CHECK(visibility(first) == plain);
    CHECK(visibility(apart) == plain);
    const Texture over = *device.createTexture(desc, {}, { heap, 0 });
    CHECK(visibility(first) == aliasing);
    CHECK(visibility(over) == aliasing);
    CHECK(visibility(apart) == plain);
    CHECK(visibility(over, Layout::CopySrc) == plain);
    for (CommandList list : lists)
    {
        list.end();
    }
    submitAndWait(device, lists);
    lists.clear();
    device.destroy(over);
    device.destroy(first);
    // The texture that does not overlap stays plain after the others are gone.
    device.beginFrame();
    CHECK(visibility(apart) == plain);
    for (CommandList list : lists)
    {
        list.end();
    }
    submitAndWait(device, lists);
    device.destroy(apart);
    device.destroy(heap);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an encoder-level barrier keeps the destination stages no command has run in yet for later encoders", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    const Buffer x = *device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.x" });
    const Buffer y = *device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.y" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Compute, "test.list");
    list.barrier(std::array{ copyToCopy() });
    // The kernel fill opens the encoder, whose opening barrier covers both of its stages; the copy writes x.
    list.fillBuffer(y, 0, 256, 0x01020304u);
    list.copyBuffer(a, 0, x, 0, 256);
    CHECK(countsOf(list).nativeBarriers == 1);
    list.barrier(
        std::array{ GlobalBarrier{ .srcStage = Stage::Copy,
                                   .srcAccess = Access::CopyWrite,
                                   .dstStage = Stage::Copy | Stage::ComputeShader,
                                   .dstAccess = Access::CopyRead | Access::CopyWrite } }
    );
    CHECK(barriersOf(list).deferredDestination == metal4::kComputeStages);
    // A copy is a blit: the barrier is emitted for the blit stage and still held for the dispatch stage.
    list.copyBuffer(a, 0, b, 0, 256);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(barriersOf(list).deferredDestination == MTL::StageDispatch);
    // The scope boundary closes the encoder before any dispatch: the dispatch stage joins the pending barrier, and the next
    // encoder emits it at queue level before its kernel fill of x.
    list.beginScope("test.next");
    CHECK(barriersOf(list).deferredDestination == 0);
    CHECK(barriersOf(list).pendingDestination == MTL::StageDispatch);
    list.fillBuffer(x, 0, 256, 0x05060708u);
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeBarriers == 3);
    CHECK(barriersOf(list).openDestination == MTL::StageDispatch);
    CHECK(barriersOf(list).pendingDestination == 0);
    list.endScope();
    list.end();
    submitAndWait(device, std::array{ list }, QueueType::Compute);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an IndirectArgs barrier is emitted in its dispatch part by a counted draw's kernel and the rest by the pass", "[metal4]")
{
    test::PassKit kit;
    CommandList list = kit.list;
    const std::vector<std::byte> zeroes(16 * sizeof(DrawIndirectArgs), std::byte{ 0 });
    const Buffer args = *kit.device.createBuffer(
        BufferDesc{ .size = zeroes.size(), .usage = BufferUsage::IndirectArgs | BufferUsage::ShaderWrite, .label = "test.args" }, zeroes
    );
    list.barrier(
        {},
        std::array{ BufferBarrier{ .buffer = args,
                                   .srcStage = Stage::ComputeShader,
                                   .srcAccess = Access::ShaderWrite,
                                   .dstStage = Stage::IndirectArgs,
                                   .dstAccess = Access::IndirectArgs } }
    );
    // IndirectArgs names every stage that consumes arguments.
    constexpr MTL::Stages kArgumentStages = MTL::StageDispatch | MTL::StageVertex | MTL::StageObject | MTL::StageMesh;
    CHECK(barriersOf(list).pendingSource == MTL::StageDispatch);
    CHECK(barriersOf(list).pendingDestination == kArgumentStages);
    CHECK(barriersOf(list).pendingVisibility == MTL4::VisibilityOptionDevice);
    kit.readyToDraw();
    SECTION("the counted draw before the pass's first draw")
    {
        list.drawIndirect(kit.device.address(args), 16);
        // The kernel's compute encoder emits the dispatch part at its opening; the render stages stay pending, joined by the
        // backend barrier from the kernel to the vertex stage, where the execute reads what it wrote.
        CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
        CHECK(countsOf(list).nativeBarriers == 1);
        CHECK(barriersOf(list).openSource == MTL::StageDispatch);
        CHECK(barriersOf(list).openDestination == MTL::StageDispatch);
        CHECK(barriersOf(list).pendingSource == MTL::StageDispatch);
        CHECK(barriersOf(list).pendingDestination == (MTL::StageVertex | MTL::StageObject | MTL::StageMesh));
        CHECK(barriersOf(list).pendingVisibility == MTL4::VisibilityOptionDevice);
        // The pass's render encoder emits the rest at its opening.
        list.draw(3);
        CHECK(encodersOf(list).kind() == metal4::EncoderKind::Render);
        CHECK(countsOf(list).nativeBarriers == 2);
        CHECK(barriersOf(list).openSource == MTL::StageDispatch);
        CHECK(barriersOf(list).openDestination == (MTL::StageVertex | MTL::StageObject | MTL::StageMesh));
        CHECK(barriersOf(list).openVisibility == MTL4::VisibilityOptionDevice);
        CHECK(barriersOf(list).pendingDestination == 0);
        list.endRenderPass();
        // The pass ran no object or mesh work, so those stages are owed again.
        CHECK(barriersOf(list).pendingSource == MTL::StageDispatch);
        CHECK(barriersOf(list).pendingDestination == (MTL::StageObject | MTL::StageMesh));
        CHECK(barriersOf(list).pendingVisibility == MTL4::VisibilityOptionDevice);
        CHECK(countsOf(list).encodersOpened == 2);
    }
    SECTION("the counted draw after a direct draw, which splits the pass")
    {
        // The direct draw opens the render encoder, which emits the render part; the dispatch part stays pending.
        list.draw(3);
        CHECK(countsOf(list).nativeBarriers == 1);
        CHECK(barriersOf(list).openSource == MTL::StageDispatch);
        CHECK(barriersOf(list).openDestination == (MTL::StageVertex | MTL::StageObject | MTL::StageMesh));
        CHECK(barriersOf(list).pendingDestination == MTL::StageDispatch);
        list.drawIndirect(kit.device.address(args), 16);
        // The split returns the object and mesh stages the ended encoder never ran; the kernel's encoder emits the dispatch
        // part; the kernel's barrier to the vertex stage and the split's from the stores to the resumed loads join them.
        CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
        CHECK(countsOf(list).nativeBarriers == 2);
        CHECK(barriersOf(list).openSource == MTL::StageDispatch);
        CHECK(barriersOf(list).openDestination == MTL::StageDispatch);
        CHECK(barriersOf(list).pendingSource == (MTL::StageDispatch | MTL::StageFragment));
        CHECK(barriersOf(list).pendingDestination == metal4::kRenderStages);
        CHECK(barriersOf(list).pendingVisibility == MTL4::VisibilityOptionDevice);
        // The resumed encoder emits it all at its opening.
        list.draw(3);
        CHECK(countsOf(list).nativeBarriers == 3);
        CHECK(barriersOf(list).openSource == (MTL::StageDispatch | MTL::StageFragment));
        CHECK(barriersOf(list).openDestination == metal4::kRenderStages);
        CHECK(barriersOf(list).pendingDestination == 0);
        list.endRenderPass();
        CHECK(barriersOf(list).pendingSource == (MTL::StageDispatch | MTL::StageFragment));
        CHECK(barriersOf(list).pendingDestination == (MTL::StageObject | MTL::StageMesh));
        CHECK(countsOf(list).encodersOpened == 3);
    }
    const core::RecordCounts before = countsOf(list);
    list.end();
    // The pass's encoder waited for the dispatch stage at its opening and ran a vertex command, so it carries what is owed:
    // the IndirectArgs destination holds the stages left back with barrierAfterStages, then endEncoding; then
    // endCommandBuffer.
    CHECK(countsOf(list).encodersOpened == before.encodersOpened);
    CHECK(countsOf(list).nativeBarriers == before.nativeBarriers + 1);
    CHECK(countsOf(list).nativeRecordCalls == before.nativeRecordCalls + 1 + 1 + 1);
    CHECK(barriersOf(list).pendingDestination == 0);
    submitAndWait(kit.device, std::array{ list });
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a barrier from a pass into Copy followed only by copies owes its fragment part until end", "[metal4]")
{
    test::PassKit kit;
    CommandList list = kit.list;
    const Texture target = kit.target();
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(test::fullPass(colors));
    list.endRenderPass();
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 0);
    list.barrier(
        {},
        {},
        std::array{ TextureBarrier{ .texture = target,
                                    .srcStage = Stage::ColorAttachment,
                                    .srcAccess = Access::ColorAttachmentWrite,
                                    .dstStage = Stage::Copy,
                                    .dstAccess = Access::CopyRead,
                                    .oldLayout = Layout::ColorAttachment,
                                    .newLayout = Layout::CopySrc } }
    );
    CHECK(barriersOf(list).pendingSource == MTL::StageFragment);
    CHECK(barriersOf(list).pendingDestination == kCopyStages);
    CHECK(barriersOf(list).pendingVisibility == MTL4::VisibilityOptionDevice);
    const Buffer readback = kit.buffer(BufferUsage::None, uint64_t{ test::PassKit::kSize } * test::PassKit::kSize * 4, Memory::Readback);
    list.copyTextureToBuffer(
        target, TextureRegion{ .extent = { test::PassKit::kSize, test::PassKit::kSize, 1 } }, readback, BufferTextureLayout{ .rowPitch = 256 }
    );
    // The compute encoder emits the blit and dispatch part; the fragment part, for a clear's load action, stays owed.
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(barriersOf(list).openSource == MTL::StageFragment);
    CHECK(barriersOf(list).openDestination == metal4::kComputeStages);
    CHECK(barriersOf(list).pendingSource == MTL::StageFragment);
    CHECK(barriersOf(list).pendingDestination == MTL::StageFragment);
    const core::RecordCounts before = countsOf(list);
    list.end();
    // The copy's encoder waited for the fragment source at its opening and ran a blit, so it carries what is owed:
    // updateFence for the dispatch stage, barrierAfterStages for the fragment stage, then endEncoding; then endCommandBuffer.
    CHECK(countsOf(list).encodersOpened == before.encodersOpened);
    CHECK(countsOf(list).nativeBarriers == before.nativeBarriers + 1);
    CHECK(countsOf(list).nativeRecordCalls == before.nativeRecordCalls + 2 + 1 + 1);
    CHECK(barriersOf(list).pendingDestination == 0);
    submitAndWait(kit.device, std::array{ list });
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a barrier after a fill stays on the open compute encoder for the dispatches that follow", "[metal4]")
{
    test::PassKit kit;
    CommandList list = kit.list;
    const Buffer counter = kit.buffer(BufferUsage::ShaderWrite, 256);
    list.fillBuffer(counter, 0, 256, 0);
    CHECK(countsOf(list).encodersOpened == 1);
    const uint64_t calls = countsOf(list).nativeRecordCalls;
    list.barrier(
        std::array{
            GlobalBarrier{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderRead } }
    );
    // The fill's encoder ran no dispatch, so it keeps the barrier: barrierAfterQueueStages for earlier encoders' work now,
    // and the encoder-level part held back for the first dispatch. It does not end.
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(countsOf(list).nativeRecordCalls - calls == 1);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(barriersOf(list).openSource == kCopyStages);
    CHECK(barriersOf(list).openDestination == MTL::StageDispatch);
    CHECK(barriersOf(list).deferredDestination == MTL::StageDispatch);
    CHECK(barriersOf(list).pendingDestination == 0);
    list.setPipeline(kit.compute());
    list.dispatch(1, 1, 1);
    // setComputePipelineState, waitForFence, barrierAfterEncoderStages and the dispatch, on the same encoder.
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(countsOf(list).nativeRecordCalls - calls == 1 + 4);
    CHECK(barriersOf(list).deferredDestination == 0);
    // A barrier between dispatches is covered by the encoder's barrier and stays in it.
    list.barrier(
        std::array{ GlobalBarrier{
            .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderRead } }
    );
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(barriersOf(list).deferredDestination == MTL::StageDispatch);
    list.dispatch(1, 1, 1);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 3);
    list.end();
    CHECK(countsOf(list).encodersOpened == 1);
    submitAndWait(kit.device, std::array{ list });
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a barrier after a dispatch whose sources lie outside the encoder still ends it", "[metal4]")
{
    test::PassKit kit;
    CommandList list = kit.list;
    list.setPipeline(kit.compute());
    list.dispatch(1, 1, 1);
    // The encoder opened with no barrier and ran a dispatch before this one, so a queue-level barrier inside it could not
    // stand for an opening barrier for the dispatch stage.
    list.barrier(
        std::array{ GlobalBarrier{
            .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderRead } }
    );
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    CHECK(barriersOf(list).pendingDestination == MTL::StageDispatch);
    list.dispatch(1, 1, 1);
    CHECK(countsOf(list).encodersOpened == 2);
    list.end();
    submitAndWait(kit.device, std::array{ list });
    destroyDevice(kit.device);
}
