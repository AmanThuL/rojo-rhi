//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4CarryTests.cpp
/// @brief Tests how Metal 4 carries a barrier owed at `CommandList::end` to later lists, with the counted-draw kernels exempt.
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
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
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
    /// What `list` has counted so far, after checking that every scope it recorded holds a timestamp pair.
    const core::RecordCounts& countsOf(CommandList list)
    {
        return test::timedCounts(list);
    }

    //==================================================================================================================
    /// A global barrier from `srcStage` with `srcAccess` to `dstStage` with `dstAccess`.
    GlobalBarrier global(Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
    {
        return GlobalBarrier{ .srcStage = srcStage, .srcAccess = srcAccess, .dstStage = dstStage, .dstAccess = dstAccess };
    }

    //==================================================================================================================
    /// Records on `list` a barrier on `buffer` from `srcStage` with `srcAccess` to `dstStage` with `dstAccess`. A buffer
    /// barrier adds no alias visibility, which would keep the dispatch stage out of the carry fence.
    void order(CommandList list, Buffer buffer, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
    {
        list.barrier(
            {}, std::array{ BufferBarrier{ .buffer = buffer, .srcStage = srcStage, .srcAccess = srcAccess, .dstStage = dstStage, .dstAccess = dstAccess } }
        );
    }

    //==================================================================================================================
    /// Submits the ended `list` to the queue of `type`, waits for it on the host and ends the frame.
    void submitAndWait(Device device, CommandList list, QueueType type = QueueType::Graphics)
    {
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(type).submit(std::array{ list }, {}, signal);
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
    }

    //==================================================================================================================
    /// Begins and ends a pass over `target` that clears it and draws nothing, so its encoder runs only load and store actions.
    void emptyPass(CommandList list, Texture target)
    {
        const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
        list.beginRenderPass(test::fullPass(colors));
        list.endRenderPass();
    }

    //==================================================================================================================
    /// Ends and submits `kit.list`, waits for it, ends the frame, opens the next and acquires a new graphics list into
    /// `kit.list`.
    void nextFrame(test::PassKit& kit)
    {
        kit.list.end();
        submitAndWait(kit.device, kit.list);
        kit.device.beginFrame();
        kit.list = kit.device.acquireCommandList(QueueType::Graphics, "test.next");
    }

    //==================================================================================================================
    /// Records a counted draw on `kit.list`, which runs its kernels on a kernels-only encoder, then moves to the next frame:
    /// from there on the device's graphics lists carry a closing batch's dispatch stage through the carry fence.
    void runCountedDraw(test::PassKit& kit)
    {
        const std::vector<std::byte> zeroes(16 * sizeof(DrawIndirectArgs), std::byte{ 0 });
        const Buffer args = *kit.device.createBuffer(
            BufferDesc{ .size = zeroes.size(), .usage = BufferUsage::IndirectArgs | BufferUsage::ShaderWrite, .label = "test.args" }, zeroes
        );
        kit.readyToDraw();
        kit.list.drawIndirect(kit.device.address(args), 16);
        kit.list.endRenderPass();
        nextFrame(kit);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a graphics list carries the dispatch stage in a barrier until a counted draw has run in an earlier frame", "[metal4]")
{
    test::PassKit kit(QueueType::Graphics, false, true);
    const Buffer ordered = kit.buffer(BufferUsage::ShaderWrite);
    const Texture target = kit.target();
    // A closing batch into a compute stage that the pass's encoder covers.
    const auto recordCarried = [&](CommandList list) {
        order(list, ordered, Stage::FragmentShader, Access::None, Stage::ColorAttachment, Access::None);
        emptyPass(list, target);
        order(list, ordered, Stage::ColorAttachment, Access::None, Stage::ComputeShader, Access::ShaderRead);
    };
    // No counted draw has run on the device: no later encoder skips a barrier to the dispatch stage, so the pass's encoder
    // emits barrierAfterStages for it, and no compute encoder waits on the carry fence.
    recordCarried(kit.list);
    core::RecordCounts before = countsOf(kit.list);
    kit.list.end();
    // barrierAfterStages, endEncoding and endCommandBuffer: no updateFence.
    CHECK(countsOf(kit.list).nativeBarriers == before.nativeBarriers + 1);
    CHECK(countsOf(kit.list).nativeRecordCalls == before.nativeRecordCalls + 3);
    submitAndWait(kit.device, kit.list);
    kit.device.beginFrame();
    kit.list = kit.device.acquireCommandList(QueueType::Graphics, "test.unfenced");
    kit.list.setPipeline(kit.compute());
    uint64_t calls = countsOf(kit.list).nativeRecordCalls;
    kit.list.dispatch(1, 1, 1);
    CHECK_FALSE(encodersOf(kit.list).carryWaited());
    // The dispatch alone, on the encoder setPipeline opened: no waitForFence.
    CHECK(countsOf(kit.list).nativeRecordCalls - calls == 1);
    // A counted draw in this frame leaves this frame's lists as they are.
    runCountedDraw(kit);
    // From the next frame on, the closing batch's dispatch stage goes through the fence and a dispatch waits on it.
    recordCarried(kit.list);
    before = countsOf(kit.list);
    kit.list.end();
    // updateFence, endEncoding and endCommandBuffer: no barrier.
    CHECK(countsOf(kit.list).nativeBarriers == before.nativeBarriers);
    CHECK(countsOf(kit.list).nativeRecordCalls == before.nativeRecordCalls + 3);
    submitAndWait(kit.device, kit.list);
    kit.device.beginFrame();
    kit.list = kit.device.acquireCommandList(QueueType::Graphics, "test.fenced");
    kit.list.setPipeline(kit.compute());
    calls = countsOf(kit.list).nativeRecordCalls;
    kit.list.dispatch(1, 1, 1);
    CHECK(encodersOf(kit.list).carryWaited());
    // waitForFence, then the dispatch.
    CHECK(countsOf(kit.list).nativeRecordCalls - calls == 2);
    kit.list.end();
    submitAndWait(kit.device, kit.list);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a pass's encoder stays open after endRenderPass and a barrier after it neither closes nor defers", "[metal4]")
{
    test::PassKit kit(QueueType::Graphics, false, true);
    CommandList list = kit.list;
    emptyPass(list, kit.target());
    CHECK(encodersOf(list).lingering());
    CHECK(encodersOf(list).renderEncoder() == nullptr);
    CHECK(countsOf(list).encodersOpened == 1);
    list.barrier(std::array{ global(Stage::ColorAttachment, Access::ColorAttachmentWrite, Stage::ComputeShader, Access::ShaderRead) });
    CHECK(encodersOf(list).lingering());
    CHECK(barriersOf(list).deferredDestination == 0);
    CHECK(barriersOf(list).pendingSource == MTL::StageFragment);
    CHECK(barriersOf(list).pendingDestination == MTL::StageDispatch);
    // The next command that needs an encoder ends the pass's encoder first.
    const Buffer a = kit.buffer(BufferUsage::None, 256);
    const Buffer b = kit.buffer(BufferUsage::None, 256);
    list.copyBuffer(a, 0, b, 0, 128);
    CHECK_FALSE(encodersOf(list).lingering());
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(countsOf(list).encodersOpened == 2);
    list.end();
    submitAndWait(kit.device, list);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a pass's closing batch whose sources its opening barrier waited for is carried by the pass's encoder", "[metal4]")
{
    test::PassKit kit(QueueType::Graphics, false, true);
    // A counted draw in an earlier frame makes the device's graphics lists use the carry fence.
    runCountedDraw(kit);
    CommandList list = kit.list;
    const Buffer ordered = kit.buffer(BufferUsage::ShaderWrite);
    // The batch before the pass names the fragment stage as a source, so the pass's encoder waits for all earlier fragment work.
    order(list, ordered, Stage::FragmentShader, Access::None, Stage::ColorAttachment, Access::None);
    emptyPass(list, kit.target());
    CHECK(barriersOf(list).openSource == MTL::StageFragment);
    SECTION("into a compute stage: the carry fence alone")
    {
        order(list, ordered, Stage::ColorAttachment, Access::None, Stage::ComputeShader, Access::ShaderRead);
        const core::RecordCounts before = countsOf(list);
        list.end();
        // updateFence, endEncoding and endCommandBuffer: no barrier and no trailing encoder.
        CHECK(countsOf(list).encodersOpened == before.encodersOpened);
        CHECK(countsOf(list).nativeBarriers == before.nativeBarriers);
        CHECK(countsOf(list).nativeRecordCalls == before.nativeRecordCalls + 3);
    }
    SECTION("into a render stage: a producer barrier on the encoder")
    {
        order(list, ordered, Stage::ColorAttachment, Access::None, Stage::FragmentShader, Access::ShaderRead);
        const core::RecordCounts before = countsOf(list);
        list.end();
        // barrierAfterStages, endEncoding and endCommandBuffer.
        CHECK(countsOf(list).encodersOpened == before.encodersOpened);
        CHECK(countsOf(list).nativeBarriers == before.nativeBarriers + 1);
        CHECK(countsOf(list).nativeRecordCalls == before.nativeRecordCalls + 3);
    }
    SECTION("into Copy: the fence for the dispatch stage and a producer barrier for the rest")
    {
        order(list, ordered, Stage::ColorAttachment, Access::None, Stage::Copy, Access::CopyRead);
        const core::RecordCounts before = countsOf(list);
        list.end();
        // updateFence, barrierAfterStages, endEncoding and endCommandBuffer.
        CHECK(countsOf(list).encodersOpened == before.encodersOpened);
        CHECK(countsOf(list).nativeBarriers == before.nativeBarriers + 1);
        CHECK(countsOf(list).nativeRecordCalls == before.nativeRecordCalls + 4);
    }
    CHECK_FALSE(encodersOf(list).lingering());
    CHECK(barriersOf(list).pendingDestination == 0);
    submitAndWait(kit.device, list);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a closing batch whose sources may lie before the closing encoder takes the trailing encoder, with the carry fence", "[metal4]")
{
    test::PassKit kit(QueueType::Graphics, false, true);
    // A counted draw in an earlier frame makes the device's graphics lists use the carry fence.
    runCountedDraw(kit);
    CommandList list = kit.list;
    const uint64_t label = test::encoderLabel(list);
    const Buffer ordered = kit.buffer(BufferUsage::ShaderWrite);
    // No batch precedes the pass, so its encoder's opening covered nothing and an earlier list's fragment work is unordered.
    emptyPass(list, kit.target());
    CHECK(barriersOf(list).openSource == 0);
    SECTION("into a compute stage")
    {
        order(list, ordered, Stage::ColorAttachment, Access::None, Stage::ComputeShader, Access::ShaderRead);
        const core::RecordCounts before = countsOf(list);
        list.end();
        // endEncoding; the trailing encoder's factory, label, queue-level barrier, updateFence and endEncoding; then
        // endCommandBuffer. No producer barrier holds back later dispatches.
        CHECK(countsOf(list).encodersOpened == before.encodersOpened + 1);
        CHECK(countsOf(list).nativeBarriers == before.nativeBarriers + 1);
        CHECK(countsOf(list).nativeRecordCalls == before.nativeRecordCalls + 1 + 4 + label + 1);
    }
    SECTION("into Copy")
    {
        order(list, ordered, Stage::ColorAttachment, Access::None, Stage::Copy, Access::CopyRead);
        const core::RecordCounts before = countsOf(list);
        list.end();
        // As above, with a producer barrier for the blit and fragment stages.
        CHECK(countsOf(list).encodersOpened == before.encodersOpened + 1);
        CHECK(countsOf(list).nativeBarriers == before.nativeBarriers + 2);
        CHECK(countsOf(list).nativeRecordCalls == before.nativeRecordCalls + 1 + 5 + label + 1);
    }
    CHECK(barriersOf(list).pendingDestination == 0);
    submitAndWait(kit.device, list);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("Compute and Copy lists carry a covered closing batch with a producer barrier on the closing encoder", "[metal4]")
{
    const QueueType type = GENERATE(QueueType::Compute, QueueType::Copy);
    CAPTURE(static_cast<int>(type));
    Device device = *createDevice({ .scopeTimings = true });
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(type, "test.list");
    const GlobalBarrier copyToCopy = global(Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyRead | Access::CopyWrite);
    list.barrier(std::array{ copyToCopy });
    list.copyBuffer(a, 0, b, 0, 128);
    list.barrier(std::array{ copyToCopy });
    const core::RecordCounts before = countsOf(list);
    list.end();
    // barrierAfterStages, endEncoding and endCommandBuffer, on the encoder that ran the copy.
    CHECK(countsOf(list).encodersOpened == before.encodersOpened);
    CHECK(countsOf(list).nativeBarriers == before.nativeBarriers + 1);
    CHECK(countsOf(list).nativeRecordCalls == before.nativeRecordCalls + 3);
    CHECK(barriersOf(list).pendingDestination == 0);
    submitAndWait(device, list, type);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a closing batch into IndirectArgs is carried by a producer barrier that holds the dispatch stage", "[metal4]")
{
    const bool arguments = GENERATE(false, true);
    CAPTURE(arguments);
    test::PassKit kit(QueueType::Graphics, false, true);
    // A counted draw in an earlier frame makes the device's graphics lists use the carry fence.
    runCountedDraw(kit);
    CommandList list = kit.list;
    const Buffer ordered = kit.buffer(BufferUsage::ShaderWrite);
    list.setPipeline(kit.compute());
    // The dispatch's encoder waits at queue level for earlier dispatches, so the closing batch is covered.
    order(list, ordered, Stage::ComputeShader, Access::ShaderWrite, Stage::ComputeShader, Access::ShaderRead | Access::ShaderWrite);
    list.dispatch(1, 1, 1);
    CHECK(barriersOf(list).openSource == MTL::StageDispatch);
    if (arguments)
    {
        order(list, ordered, Stage::ComputeShader, Access::ShaderWrite, Stage::IndirectArgs, Access::IndirectArgs);
    }
    else
    {
        order(list, ordered, Stage::ComputeShader, Access::ShaderWrite, Stage::ComputeShader, Access::ShaderRead);
    }
    const core::RecordCounts before = countsOf(list);
    list.end();
    CHECK(countsOf(list).encodersOpened == before.encodersOpened);
    // A counted draw's kernels read records, so an IndirectArgs destination holds them back with the producer barrier; a
    // ComputeShader destination reaches only the dispatches that wait on the carry fence.
    CHECK(countsOf(list).nativeBarriers == before.nativeBarriers + (arguments ? 1 : 0));
    CHECK(countsOf(list).nativeRecordCalls == before.nativeRecordCalls + 3);
    submitAndWait(kit.device, list);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a graphics compute encoder waits on the carry fence once, before its first dispatch", "[metal4]")
{
    test::PassKit kit(QueueType::Graphics, false, true);
    // A counted draw in an earlier frame makes the device's graphics lists use the carry fence.
    runCountedDraw(kit);
    CommandList list = kit.list;
    const uint64_t label = test::encoderLabel(list);
    list.setPipeline(kit.compute());
    CHECK_FALSE(encodersOf(list).carryWaited());
    uint64_t before = countsOf(list).nativeRecordCalls;
    list.dispatch(1, 1, 1);
    // waitForFence, then the dispatch.
    CHECK(countsOf(list).nativeRecordCalls - before == 2);
    CHECK(encodersOf(list).carryWaited());
    before = countsOf(list).nativeRecordCalls;
    list.dispatch(1, 1, 1);
    CHECK(countsOf(list).nativeRecordCalls - before == 1);
    // A copy runs in the blit stage, which the fence does not carry: a new encoder that only copies does not wait.
    list.beginScope("test.copies");
    const Buffer a = kit.buffer(BufferUsage::None, 256);
    const Buffer b = kit.buffer(BufferUsage::None, 256);
    list.copyBuffer(a, 0, b, 0, 128);
    CHECK_FALSE(encodersOf(list).carryWaited());
    list.endScope();
    // Every encoder that dispatches waits, not only the list's first.
    list.beginScope("test.second");
    before = countsOf(list).nativeRecordCalls;
    list.dispatch(1, 1, 1);
    // The open (factory, label, table), the pipeline, waitForFence and the dispatch.
    CHECK(countsOf(list).nativeRecordCalls - before == 2 + label + 1 + 1 + 1);
    CHECK(encodersOf(list).carryWaited());
    list.endScope();
    list.end();
    submitAndWait(kit.device, list);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("a counted draw's kernels encoder does not wait on the carry fence, and a Compute list has none", "[metal4]")
{
    SECTION("the kernels of a counted draw")
    {
        test::PassKit kit(QueueType::Graphics, false, true);
        CommandList list = kit.list;
        const std::vector<std::byte> zeroes(16 * sizeof(DrawIndirectArgs), std::byte{ 0 });
        const Buffer args = *kit.device.createBuffer(
            BufferDesc{ .size = zeroes.size(), .usage = BufferUsage::IndirectArgs | BufferUsage::ShaderWrite, .label = "test.args" }, zeroes
        );
        kit.readyToDraw();
        list.drawIndirect(kit.device.address(args), 16);
        CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
        CHECK_FALSE(encodersOf(list).carryWaited());
        list.endRenderPass();
        list.end();
        submitAndWait(kit.device, list);
        destroyDevice(kit.device);
    }
    SECTION("a dispatch on a Compute list")
    {
        test::PassKit kit(QueueType::Compute, false, true);
        CommandList list = kit.list;
        list.setPipeline(kit.compute());
        const uint64_t before = countsOf(list).nativeRecordCalls;
        list.dispatch(1, 1, 1);
        CHECK(countsOf(list).nativeRecordCalls - before == 1);
        CHECK_FALSE(encodersOf(list).carryWaited());
        list.end();
        submitAndWait(kit.device, list, QueueType::Compute);
        destroyDevice(kit.device);
    }
}

//======================================================================================================================
TEST_CASE("a closing batch whose visibility includes ResourceAlias keeps the dispatch stage in a barrier", "[metal4]")
{
    const bool covered = GENERATE(false, true);
    CAPTURE(covered);
    test::PassKit kit(QueueType::Graphics, false, true);
    // A counted draw in an earlier frame makes the device's graphics lists use the carry fence.
    runCountedDraw(kit);
    CommandList list = kit.list;
    const uint64_t label = test::encoderLabel(list);
    if (covered)
    {
        list.barrier(std::array{ global(Stage::FragmentShader, Access::None, Stage::ColorAttachment, Access::ColorAttachmentWrite) });
    }
    emptyPass(list, kit.target());
    list.barrier(std::array{ global(Stage::ColorAttachment, Access::ColorAttachmentWrite, Stage::ComputeShader, Access::ShaderRead) });
    // However the batches decide alias visibility, a pending barrier that holds it needs a flush the fence cannot give.
    metal4::BarrierState& barriers = CommandListImpl::of(list).encoders.barriers();
    barriers.pendingVisibility |= MTL4::VisibilityOptionResourceAlias;
    const core::RecordCounts before = countsOf(list);
    list.end();
    if (covered)
    {
        // barrierAfterStages to the dispatch stage on the pass's encoder, endEncoding and endCommandBuffer: no updateFence.
        CHECK(countsOf(list).encodersOpened == before.encodersOpened);
        CHECK(countsOf(list).nativeBarriers == before.nativeBarriers + 1);
        CHECK(countsOf(list).nativeRecordCalls == before.nativeRecordCalls + 3);
    }
    else
    {
        // endEncoding; the trailing encoder's factory, label, queue-level barrier, barrierAfterStages and endEncoding; then
        // endCommandBuffer: no updateFence.
        CHECK(countsOf(list).encodersOpened == before.encodersOpened + 1);
        CHECK(countsOf(list).nativeBarriers == before.nativeBarriers + 2);
        CHECK(countsOf(list).nativeRecordCalls == before.nativeRecordCalls + 1 + 4 + label + 1);
    }
    CHECK(barriersOf(list).pendingDestination == 0);
    submitAndWait(kit.device, list);
    destroyDevice(kit.device);
}
