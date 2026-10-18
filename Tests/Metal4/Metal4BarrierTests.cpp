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

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Metal/Metal.hpp>

#include <array>
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
    /// What `list` has counted so far.
    const core::RecordCounts& countsOf(CommandList list)
    {
        return core::coreOf(list).counts;
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

    //==================================================================================================================
    /// The native texture `texture` wraps.
    MTL::Texture* nativeOf(Device device, Texture texture)
    {
        return static_cast<MTL::Texture*>(DeviceImpl::of(device).core.pool<Texture>().native(texture));
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
    list.barrier(std::array{ copyToCopy() });
    // Nothing is emitted yet: no encoder is open, so the barrier waits for one.
    CHECK(countsOf(list).nativeBarriers == 0);
    CHECK(countsOf(list).nativeRecordCalls == 2);
    CHECK(countsOf(list).globalBarriers == 1);
    CHECK(barriersOf(list).pendingSource == MTL::StageBlit);
    CHECK(barriersOf(list).pendingDestination == MTL::StageBlit);
    CHECK(barriersOf(list).pendingVisibility == (MTL4::VisibilityOptionDevice | MTL4::VisibilityOptionResourceAlias));
    list.copyBuffer(a, 0, b, 0, 256);
    // The opening adds computeCommandEncoder, setLabel, setArgumentTable and barrierAfterQueueStages; then the copy.
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 2 + 4 + 1);
    CHECK(barriersOf(list).pendingDestination == 0);
    CHECK(barriersOf(list).pendingSource == 0);
    CHECK(barriersOf(list).pendingVisibility == 0);
    CHECK(barriersOf(list).openSource == MTL::StageBlit);
    CHECK(barriersOf(list).openDestination == MTL::StageBlit);
    list.end();
    // endEncoding and endCommandBuffer; nothing is pending, so no trailing encoder.
    CHECK(countsOf(list).nativeRecordCalls == 7 + 2);
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
    list.barrier(std::array{ copyToCopy() });
    list.copyBuffer(a, 0, b, 0, 128);
    CHECK(countsOf(list).nativeRecordCalls == 7);
    list.barrier(std::array{ copyToCopy() });
    // The encoder stays open and holds the barrier back until a command follows it in the encoder.
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(encodersOf(list).holdsWork());
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 7);
    CHECK(barriersOf(list).pendingDestination == 0);
    CHECK(barriersOf(list).deferredDestination == MTL::StageBlit);
    // A second barrier with no command between them merges with the first.
    list.barrier(std::array{ copyToCopy() });
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 7);
    // barrierAfterEncoderStages goes before the copy that follows it.
    list.copyBuffer(b, 0, a, 0, 128);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(countsOf(list).nativeRecordCalls == 7 + 1 + 1);
    CHECK(barriersOf(list).deferredDestination == 0);
    list.end();
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(countsOf(list).nativeRecordCalls == 9 + 2);
    submitAndWait(device, std::array{ list }, QueueType::Copy);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an encoder-level barrier with no command after it is emitted at end as the trailing barrier", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Copy, "test.list");
    list.barrier(std::array{ copyToCopy() });
    list.copyBuffer(a, 0, b, 0, 128);
    list.barrier(std::array{ copyToCopy() });
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 7);
    list.end();
    // endEncoding, which hands the held barrier to the queue level; the trailing encoder's five calls; endCommandBuffer.
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeBarriers == 1 + 2);
    CHECK(countsOf(list).nativeRecordCalls == 7 + 1 + 5 + 1);
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
    list.copyBuffer(a, 0, b, 0, 128);
    // The encoder opened with no barrier, so its opening covered nothing.
    CHECK(barriersOf(list).openSource == 0);
    CHECK(barriersOf(list).openDestination == 0);
    CHECK(countsOf(list).nativeRecordCalls == 6);
    list.barrier(std::array{ copyToCopy() });
    // The copy's sources may include earlier lists' work, so the barrier is queue-level: the encoder ends (endEncoding).
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    CHECK(countsOf(list).nativeBarriers == 0);
    CHECK(countsOf(list).nativeRecordCalls == 6 + 1);
    CHECK(barriersOf(list).pendingSource == MTL::StageBlit);
    CHECK(barriersOf(list).pendingDestination == MTL::StageBlit);
    list.copyBuffer(b, 0, a, 0, 128);
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 7 + 4 + 1);
    CHECK(barriersOf(list).pendingDestination == 0);
    list.end();
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeRecordCalls == 12 + 2);
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
    list.barrier(std::array{ copyToCopy() });
    CHECK(barriersOf(list).pendingSource == kCopyStages);
    CHECK(barriersOf(list).pendingDestination == kCopyStages);
    list.copyBuffer(a, 0, b, 0, 128);
    // The compute encoder emits the dispatch and blit part; the fragment part stays owed.
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 7);
    CHECK(barriersOf(list).pendingSource == kCopyStages);
    CHECK(barriersOf(list).pendingDestination == MTL::StageFragment);
    CHECK(barriersOf(list).openSource == kCopyStages);
    CHECK(barriersOf(list).openDestination == metal4::kComputeStages);
    // Its sources are covered, so the next barrier stays in the encoder, and its fragment part joins what is owed.
    list.barrier(std::array{ copyToCopy() });
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).nativeRecordCalls == 7);
    CHECK(barriersOf(list).pendingDestination == MTL::StageFragment);
    list.copyBuffer(b, 0, a, 0, 128);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(countsOf(list).nativeRecordCalls == 9);
    list.end();
    // endEncoding; then the trailing encoder: computeCommandEncoder, setLabel, two barriers and endEncoding; then
    // endCommandBuffer.
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeBarriers == 4);
    CHECK(countsOf(list).nativeRecordCalls == 9 + 1 + 5 + 1);
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
        CHECK(countsOf(list).nativeRecordCalls == 2 + 4 + 1 + 2);
    }
    else
    {
        // The encoder ran no dispatch, so the dispatch stage is still owed and a trailing encoder emits it.
        CHECK(countsOf(list).encodersOpened == 2);
        CHECK(countsOf(list).nativeBarriers == 3);
        CHECK(countsOf(list).nativeRecordCalls == 2 + 4 + 1 + 1 + 5 + 1);
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
    CHECK(barriersOf(list).pendingVisibility == (MTL4::VisibilityOptionDevice | MTL4::VisibilityOptionResourceAlias));
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
    // No dispatch followed, so the barrier and the unrun dispatch stage go to the trailing encoder.
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeBarriers == 3);
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
    list.barrier({});
    CHECK(countsOf(list).globalBarriers == 0);
    CHECK(countsOf(list).nativeRecordCalls == 2);
    list.copyBuffer(a, 0, b, 0, 128);
    CHECK(countsOf(list).nativeRecordCalls == 6);
    // No earlier work to wait for: nothing to order, and the encoder stays open.
    list.barrier(std::array{ GlobalBarrier{ .srcStage = Stage::None, .dstStage = Stage::Copy } });
    CHECK(countsOf(list).globalBarriers == 1);
    // No later work to hold back: nothing to order either.
    list.barrier({}, std::array{ BufferBarrier{ .buffer = b, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::None } });
    CHECK(countsOf(list).bufferBarriers == 1);
    list.barrier({}, {}, {});
    CHECK(countsOf(list).nativeBarriers == 0);
    CHECK(countsOf(list).nativeRecordCalls == 6);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(encodersOf(list).holdsWork());
    CHECK(barriersOf(list).pendingDestination == 0);
    CHECK(barriersOf(list).pendingSource == 0);
    list.end();
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeRecordCalls == 6 + 2);
    submitAndWait(device, std::array{ list });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("visibility is Device for named accesses and adds ResourceAlias for a global barrier or a placed texture's discard", "[metal4]")
{
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer({ .size = 256, .label = "test.buffer" });
    const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" };
    const PlacementRequirements requirements = device.placementRequirements(desc);
    const Heap heap = *device.createHeap({ .size = requirements.size, .label = "test.heap" });
    const Texture placed = *device.createTexture(desc, {}, { heap, 0 });
    const Texture dedicated = *device.createTexture(desc);
    // Only placed textures are untracked, which is how the lowering tells them apart.
    REQUIRE(nativeOf(device, placed)->hazardTrackingMode() == MTL::HazardTrackingModeUntracked);
    REQUIRE(nativeOf(device, dedicated)->hazardTrackingMode() != MTL::HazardTrackingModeUntracked);
    const BufferBarrier copyRead{
        .buffer = buffer, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead
    };
    const BufferBarrier executionOnly{ .buffer = buffer, .srcStage = Stage::Copy, .dstStage = Stage::Copy };
    const GlobalBarrier globalOnly{ .srcStage = Stage::Copy, .dstStage = Stage::Copy };
    const auto discard = [](Texture texture) {
        return TextureBarrier{ .texture = texture,
                               .srcStage = Stage::Copy,
                               .dstStage = Stage::Copy,
                               .dstAccess = Access::CopyWrite,
                               .oldLayout = Layout::Undefined,
                               .newLayout = Layout::CopyDst };
    };
    const TextureBarrier keep{ .texture = placed,
                               .srcStage = Stage::Copy,
                               .srcAccess = Access::CopyRead,
                               .dstStage = Stage::Copy,
                               .dstAccess = Access::CopyWrite,
                               .oldLayout = Layout::CopySrc,
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
    CHECK(visibilityOf(std::array{ globalOnly }, {}, {}) == MTL4::VisibilityOptionResourceAlias);
    CHECK(visibilityOf(std::array{ copyToCopy() }, {}, {}) == (MTL4::VisibilityOptionDevice | MTL4::VisibilityOptionResourceAlias));
    CHECK(visibilityOf({}, {}, std::array{ discard(dedicated) }) == MTL4::VisibilityOptionDevice);
    CHECK(visibilityOf({}, {}, std::array{ discard(placed) }) == (MTL4::VisibilityOptionDevice | MTL4::VisibilityOptionResourceAlias));
    CHECK(visibilityOf({}, {}, std::array{ keep }) == MTL4::VisibilityOptionDevice);
    CHECK(visibilityOf({}, std::array{ executionOnly }, std::array{ discard(placed) }) == (MTL4::VisibilityOptionDevice | MTL4::VisibilityOptionResourceAlias));
    for (CommandList list : lists)
    {
        list.end();
    }
    submitAndWait(device, lists);
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
