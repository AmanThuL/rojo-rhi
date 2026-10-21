//----------------------------------------------------------------------------------------------------------------------
/// @file CommandListCoreTests.cpp
/// @brief Tests the state the core keeps for a command list and how the per-slot pools reuse lists.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    void submit(Device device, CommandList list, QueueType type = QueueType::Graphics)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists);
    }
} // namespace

//======================================================================================================================
TEST_CASE("an acquired list records its type, frame and label and is recording", "[core]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Compute, "test.list");
    const core::CommandListCore& state = core::coreOf(list);
    CHECK(state.type == QueueType::Compute);
    CHECK(state.frame == 1);
    CHECK(state.label == "test.list");
    CHECK(state.state == core::ListState::Recording);
    CHECK_FALSE(state.passOpen);
    CHECK(state.scopes.empty());
    submit(device, list, QueueType::Compute);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a list without a label is recorded as rojorhi.commandlist.unnamed", "[core]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, {});
    CHECK(core::coreOf(list).label == "rojorhi.commandlist.unnamed");
    submit(device, list);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("end and submit move a list through its states", "[core]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.end();
    CHECK(core::coreOf(list).state == core::ListState::Ended);
    const std::array<CommandList, 1> lists{ list };
    device.queue(QueueType::Graphics).submit(lists);
    CHECK(core::coreOf(list).state == core::ListState::Submitted);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a list is reused when its frame slot is reused and not before", "[core]")
{
    Device device = *createDevice(DeviceDesc{ .framesInFlight = 2 });
    device.beginFrame();
    CommandList inFrameOne = device.acquireCommandList(QueueType::Graphics, "frame1");
    const core::CommandListCore* const firstState = &core::coreOf(inFrameOne);
    submit(device, inFrameOne);
    device.endFrame();

    device.beginFrame();
    CommandList inFrameTwo = device.acquireCommandList(QueueType::Graphics, "frame2");
    CHECK(&core::coreOf(inFrameTwo) != firstState);
    CHECK(core::coreOf(inFrameOne).label == "frame1");
    submit(device, inFrameTwo);
    device.endFrame();

    device.beginFrame();
    CommandList inFrameThree = device.acquireCommandList(QueueType::Graphics, "frame3");
    CHECK(&core::coreOf(inFrameThree) == firstState);
    CHECK(core::coreOf(inFrameThree).frame == 3);
    CHECK(core::coreOf(inFrameThree).state == core::ListState::Recording);
    submit(device, inFrameThree);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a reused list starts empty and keeps the capacity of its scope stack", "[core]")
{
    Device device = *createDevice(DeviceDesc{ .framesInFlight = 2 });
    device.beginFrame();
    CommandList first = device.acquireCommandList(QueueType::Graphics, "frame1");
    for (int i = 0; i < 8; ++i)
    {
        first.beginScope("scope");
    }
    for (int i = 0; i < 8; ++i)
    {
        first.endScope();
    }
    const std::array<std::byte, 16> bytes = {};
    (void)first.uploadFrameData(bytes);
    const size_t capacity = core::coreOf(first).scopes.capacity();
    submit(device, first);
    device.endFrame();
    for (uint64_t frame = 2; frame <= 3; ++frame)
    {
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "next");
        if (frame == 3)
        {
            const core::CommandListCore& state = core::coreOf(list);
            CHECK(state.scopes.empty());
            CHECK(state.scopes.capacity() == capacity);
            CHECK(state.page.host == nullptr);
            CHECK(core::pageUsedBytes(state) == 0);
            CHECK(state.label == "next");
        }
        submit(device, list);
        device.endFrame();
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a reused list resets every field it recorded and keeps its containers' capacity", "[core]")
{
    Device device = *createDevice(DeviceDesc{ .framesInFlight = 2 });
    device.beginFrame();
    CommandList first = device.acquireCommandList(QueueType::Graphics, "a list label longer than any short string can hold");
    const std::array<std::byte, 16> bytes = {};
    first.setRoot(first.uploadFrameData(bytes, 16));
    first.beginScope("outer");
    first.beginScope("inner");
    first.endScope();
    first.endScope();
    const core::CommandListCore& recorded = core::coreOf(first);
    REQUIRE(recorded.scopeLog.size() == 2);
    REQUIRE(recorded.counts.frameDataUploads == 1);
    const size_t logCapacity = recorded.scopeLog.capacity();
    const size_t labelCapacity = recorded.label.capacity();
    submit(device, first);
    device.endFrame();
    for (uint64_t frame = 2; frame <= 3; ++frame)
    {
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, frame == 3 ? "a shorter label than the first, still long" : "next");
        if (frame == 3)
        {
            const core::CommandListCore& state = core::coreOf(list);
            REQUIRE(&state == &recorded);
            CHECK(state.device == &core::coreOf(device));
            CHECK(state.frame == 3);
            CHECK(state.type == QueueType::Graphics);
            CHECK(state.state == core::ListState::Recording);
            CHECK(state.label == "a shorter label than the first, still long");
            // The label is copied into the list's own string, which keeps its capacity.
            CHECK(state.label.capacity() == labelCapacity);
            CHECK(state.root.lo == 0);
            CHECK(state.root.hi == 0);
            CHECK(state.rootBuffer.bits == 0);
            CHECK_FALSE(state.passOpen);
            CHECK_FALSE(state.pipeline.bound);
            CHECK(state.pass.area.width == 0);
            CHECK(state.pass.colors.empty());
            CHECK(state.passScopeBase == 0);
            CHECK(state.page.host == nullptr);
            CHECK(state.cursor == nullptr);
            CHECK(state.limit == nullptr);
            CHECK(state.scopes.empty());
            CHECK(state.scopeLog.empty());
            CHECK(state.scopeLog.capacity() == logCapacity);
            CHECK(state.pendingAccesses.empty());
            CHECK(state.trackerLog.events.empty());
            CHECK(state.trackerLog.command == 0);
            CHECK(state.counts.frameDataUploads == 0);
            CHECK(state.counts.frameArenaBytes == 0);
            CHECK(state.counts.nativeRecordCalls == 0);
        }
        submit(device, list);
        device.endFrame();
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a slot hands out distinct lists within one frame", "[core]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList a = device.acquireCommandList(QueueType::Graphics, "a");
    CommandList b = device.acquireCommandList(QueueType::Graphics, "b");
    CHECK(&core::coreOf(a) != &core::coreOf(b));
    submit(device, a);
    submit(device, b);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the frame arena counts its pages", "[core]")
{
    Device device = *createDevice();
    core::DeviceCore& core = core::coreOf(device);
    CHECK(core.arena().growths() == 0);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::array<std::byte, 8> bytes = {};
    (void)list.uploadFrameData(bytes);
    (void)list.uploadFrameData(bytes);
    CHECK(core.arena().growths() == 1);
    submit(device, list);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a render pass records its attachments, a pipeline its fields in Debug, and both clear when the pass ends", "[core]")
{
    test::PassKit kit;
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target(Format::RGBA16Float, 4) } };
    kit.list.beginRenderPass(test::fullPass(colors));
    const core::CommandListCore& state = core::coreOf(kit.list);
    CHECK(state.passOpen);
    CHECK(state.pass.area.width == test::PassKit::kSize);
#ifndef NDEBUG
    // The attachments' formats and samples and the pipeline's fields feed the Debug-tier checks; a Release build reads no
    // record to fill them (ADR 0020).
    CHECK(state.pass.colors == std::vector<Format>{ Format::RGBA16Float });
    CHECK(state.pass.samples == 4);
#else
    CHECK(state.pass.colors.empty());
#endif
    CHECK(state.pass.depthFormat == Format::Undefined);
    CHECK_FALSE(state.pipeline.bound);
    kit.list.setViewport({ .width = 1.0f, .height = 1.0f });
    CHECK(state.pass.viewportSet);
    CHECK_FALSE(state.pass.scissorSet);
    constexpr std::array<Format, 1> formats = { Format::RGBA16Float };
    kit.list.setPipeline(kit.graphics(formats, Format::Undefined, 4, PrimitiveClass::Line, true));
#ifndef NDEBUG
    CHECK(state.pipeline.bound);
    CHECK(state.pipeline.primitive == PrimitiveClass::Line);
    CHECK(state.pipeline.depthBias);
#else
    CHECK_FALSE(state.pipeline.bound);
#endif
    kit.list.endRenderPass();
    CHECK_FALSE(state.passOpen);
    CHECK_FALSE(state.pipeline.bound);
    kit.beginSimplePass();
    CHECK_FALSE(state.pass.viewportSet);
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("scopes record which side of a pass each began on", "[core]")
{
    test::PassKit kit;
    const core::CommandListCore& state = core::coreOf(kit.list);
    kit.list.beginScope("outside");
    kit.beginSimplePass();
    kit.list.beginScope("inside");
    CHECK(state.scopes == std::vector<bool>{ false, true });
    kit.list.endScope();
    kit.list.endRenderPass();
    kit.list.endScope();
    CHECK(state.scopes.empty());
    kit.finish();
}
