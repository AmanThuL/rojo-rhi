//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4CommandListTests.cpp
/// @brief Tests Metal 4 command lists against the allocators, command buffers and argument tables they own and record into.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Support/Metal4/Metal4Timing.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// The native state of `list`.
    metal4::ListState& stateOf(CommandList list)
    {
        return CommandListImpl::of(list).state;
    }

    //==================================================================================================================
    /// The UTF-8 text of `string`, read inside a pool because a getter may autorelease.
    std::string textOf(NS::String* string)
    {
        const metal4::AutoreleasePool pool;
        return metal4::toStdString(string);
    }

    //==================================================================================================================
    /// The label of `list`'s command buffer, read inside a pool because the getter autoreleases.
    std::string commandBufferLabel(CommandList list)
    {
        const metal4::AutoreleasePool pool;
        return metal4::toStdString(stateOf(list).commandBuffer->label());
    }

    //==================================================================================================================
    /// Ends every list of `lists` and submits them to the graphics queue in one submission.
    void endAndSubmit(Device device, std::span<const CommandList> lists)
    {
        for (CommandList list : lists)
        {
            list.end();
        }
        device.queue(QueueType::Graphics).submit(lists);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a list records into its own command buffer, labelled with the list's label", "[metal4]")
{
    Device device = *createDevice();
    device.beginFrame();
    const std::array<CommandList, 2> lists{ device.acquireCommandList(QueueType::Graphics, "test.a"),
                                            device.acquireCommandList(QueueType::Graphics, "test.b") };
    const metal4::ListState& a = stateOf(lists[0]);
    const metal4::ListState& b = stateOf(lists[1]);
    REQUIRE(a.allocator != nullptr);
    REQUIRE(a.commandBuffer != nullptr);
    REQUIRE(a.table != nullptr);
    REQUIRE(b.commandBuffer != nullptr);
    CHECK(commandBufferLabel(lists[0]) == "test.a");
    CHECK(commandBufferLabel(lists[1]) == "test.b");
    CHECK(a.allocator != b.allocator);
    CHECK(a.commandBuffer != b.commandBuffer);
    CHECK(a.table != b.table);
    endAndSubmit(device, lists);
    device.endFrame();
    // A list the pool hands out again records under the label of its new acquisition.
    device.beginFrame();
    const CommandList unnamed = device.acquireCommandList(QueueType::Graphics, "");
    CHECK(commandBufferLabel(unnamed) == "rojorhi.commandlist.unnamed");
    endAndSubmit(device, std::array{ unnamed });
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a new list creates its allocator, command buffer and tables once and a reused list creates none", "[metal4]")
{
    Device device = *createDevice();
    const uint32_t slots = core::coreOf(device).framesInFlight();
    for (uint32_t frame = 0; frame < 2 * slots; ++frame)
    {
        device.beginFrame();
        const Counters before = device.counters();
        const std::array<CommandList, 2> lists{ device.acquireCommandList(QueueType::Graphics, "test.a"),
                                                device.acquireCommandList(QueueType::Graphics, "test.b") };
        const Counters after = device.counters();
        // The first frame of each slot grows its pool by two lists of four objects; later frames reuse them.
        CHECK(after.nativeCreateCalls == before.nativeCreateCalls + (frame < slots ? 8 : 0));
        CHECK(after.cpuAllocations == before.cpuAllocations + (frame < slots ? 2 : 0));
        endAndSubmit(device, lists);
        device.endFrame();
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("acquire and end count beginCommandBuffer, setLabel and endCommandBuffer", "[metal4]")
{
    Device device = *createDevice();
    device.beginFrame();
    const CommandList list = device.acquireCommandList(QueueType::Copy, "test.list");
    CHECK(core::coreOf(list).counts.nativeRecordCalls == 2);
    // A list with no command still begins, ends and commits: an empty command buffer is legal.
    CommandList(list).end();
    CHECK(core::coreOf(list).counts.nativeRecordCalls == 3);
    const Counters before = device.counters();
    const std::array<CommandList, 1> lists{ list };
    device.queue(QueueType::Copy).submit(lists);
    CHECK(device.counters().nativeRecordCalls == before.nativeRecordCalls + 3);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("submit commits the lists' command buffers and their signal completes on the GPU", "[metal4]")
{
    Device device = *createDevice();
    const Fence fence = *device.createFence(0, "test.fence");
    device.beginFrame();
    std::array<CommandList, 3> lists{};
    for (CommandList& list : lists)
    {
        list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.end();
    }
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    const Counters before = device.counters();
    device.queue(QueueType::Graphics).submit(lists, {}, signal);
    const Counters after = device.counters();
    // One commit of the three command buffers and the fence's signal; a submission with lists signals no queue progress.
    CHECK(after.nativeSubmitCalls == before.nativeSubmitCalls + 2);
    CHECK(device.wait({ fence, 1 }, UINT64_MAX));
    // A submission with no list commits nothing.
    device.queue(QueueType::Graphics).submit({});
    CHECK(device.counters().nativeSubmitCalls == after.nativeSubmitCalls);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a reused slot resets the allocator of every list it took", "[metal4]")
{
    Device device = *createDevice();
    const uint32_t slots = core::coreOf(device).framesInFlight();
    for (uint32_t frame = 0; frame < slots; ++frame)
    {
        device.beginFrame();
        // The first slot takes three lists, the others one each.
        const size_t count = frame == 0 ? 3 : 1;
        std::vector<CommandList> lists;
        for (size_t i = 0; i < count; ++i)
        {
            lists.push_back(device.acquireCommandList(QueueType::Graphics, "test.list"));
        }
        endAndSubmit(device, lists);
        device.endFrame();
    }
    // No fence was destroyed, so the slot's reuse reads no progress event: the frame wait and one reset per list.
    const Counters before = device.counters();
    device.beginFrame();
    CHECK(device.counters().nativeHostCalls == before.nativeHostCalls + 1 + 3);
    const CommandList list = device.acquireCommandList(QueueType::Graphics, "test.reused");
    CHECK(commandBufferLabel(list) == "test.reused");
    endAndSubmit(device, std::array{ list });
    device.endFrame();
    const Counters next = device.counters();
    device.beginFrame();
    CHECK(device.counters().nativeHostCalls == next.nativeHostCalls + 1 + 1);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("nested scopes push and pop debug groups on the command buffer", "[metal4]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Compute, "test.list");
    const metal4::ListState& state = stateOf(list);
    list.beginScope("test.outer");
    REQUIRE(state.scopeLabels.size() == 1);
    CHECK(textOf(state.scopeLabels.back()) == "test.outer");
    list.beginScope("");
    REQUIRE(state.scopeLabels.size() == 2);
    // An empty label takes the core's default.
    CHECK(textOf(state.scopeLabels.back()) == "rojorhi.scope.unnamed");
    CHECK(test::timedCounts(list).nativeRecordCalls == 2 + 2 * 2);
    list.endScope();
    REQUIRE(state.scopeLabels.size() == 1);
    CHECK(textOf(state.scopeLabels.back()) == "test.outer");
    list.endScope();
    CHECK(state.scopeLabels.empty());
    list.end();
    CHECK(state.scopeLabels.empty());
    CHECK(test::timedCounts(list).nativeRecordCalls == 2 + 2 * 4 + 1);
    const Fence fence = *device.createFence(0, "test.fence");
    const std::array<CommandList, 1> lists{ list };
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    device.queue(QueueType::Compute).submit(lists, {}, signal);
    CHECK(device.wait({ fence, 1 }, UINT64_MAX));
    // The compute work joins the frame's graphics work.
    const std::array<FenceValue, 1> wait{ FenceValue{ fence, 1 } };
    device.queue(QueueType::Graphics).submit({}, wait);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a scope label that is not valid UTF-8 names its debug group with the default label", "[metal4]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const metal4::ListState& state = stateOf(list);
    list.beginScope("test.\xff\xfe");
    REQUIRE(state.scopeLabels.size() == 1);
    REQUIRE(state.scopeLabels.back() != nullptr);
    CHECK(textOf(state.scopeLabels.back()) == "rojorhi.scope.unnamed");
    list.endScope();
    CHECK(state.scopeLabels.empty());
    endAndSubmit(device, std::array{ list });
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("declareAccess makes no native call", "[metal4]")
{
    // Without validation the core keeps no declaration, so the list ends with none pending.
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer({ .size = 64, .usage = BufferUsage::ShaderWrite });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.declareAccess(std::array{ ResourceAccess{ .buffer = buffer, .stage = Stage::ComputeShader, .access = Access::ShaderWrite } });
    CHECK(core::coreOf(list).counts.nativeRecordCalls == 2);
    endAndSubmit(device, std::array{ list });
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("four threads record lists of one frame at once", "[metal4]")
{
    Device device = *createDevice();
    const Fence fence = *device.createFence(0, "test.fence");
    device.beginFrame();
    std::array<CommandList, 4> lists{};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < lists.size(); ++i)
    {
        threads.emplace_back([&, i] {
            CommandList list = device.acquireCommandList(QueueType::Graphics, "test.thread");
            for (int scope = 0; scope < 16; ++scope)
            {
                list.beginScope("test.outer");
                list.beginScope("test.inner");
                list.endScope();
                list.endScope();
            }
            list.end();
            lists[i] = list;
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    for (size_t i = 0; i < lists.size(); ++i)
    {
        for (size_t j = i + 1; j < lists.size(); ++j)
        {
            CHECK(stateOf(lists[i]).commandBuffer != stateOf(lists[j]).commandBuffer);
        }
        CHECK(test::timedCounts(lists[i]).nativeRecordCalls == 2 + 16 * 8 + 1);
    }
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    device.queue(QueueType::Graphics).submit(lists, {}, signal);
    CHECK(device.wait({ fence, 1 }, UINT64_MAX));
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("destroyDevice releases every list after the queues drain", "[metal4]")
{
    Device device = *createDevice();
    const uint32_t slots = core::coreOf(device).framesInFlight();
    // Every slot holds lists, the last frame's still on the GPU when the device is destroyed.
    for (uint32_t frame = 0; frame < slots + 1; ++frame)
    {
        device.beginFrame();
        std::array<CommandList, 2> lists{ device.acquireCommandList(QueueType::Graphics, "test.a"), device.acquireCommandList(QueueType::Graphics, "test.b") };
        lists[0].beginScope("test.scope");
        lists[0].endScope();
        endAndSubmit(device, lists);
        device.endFrame();
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a list acquired again with the same label sends its command buffer no label, and a new label is set", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    // With two frame slots, frames 1, 3 and 5 take the first list of one slot's pool, frames 2 and 4 that of the other.
    struct Recorded
    {
        const CommandListImpl* list;
        uint64_t acquireCalls;
        std::string label;
        NS::String* scope;
        std::string scopeText;
    };
    const auto record = [&](std::string_view label, std::string_view scope) {
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, label);
        const uint64_t calls = CommandListImpl::of(list).core.counts.nativeRecordCalls;
        list.beginScope(scope);
        NS::String* string = stateOf(list).scopeLabels.back();
        const std::string text = textOf(string);
        list.endScope();
        const std::array lists{ list };
        endAndSubmit(device, lists);
        device.endFrame();
        return Recorded{ &CommandListImpl::of(list), calls, commandBufferLabel(list), string, text };
    };
    // Scope labels long enough not to be tagged-pointer strings, so equal pointers mean one object.
    const Recorded first = record("test.a", "test.scope.with.a.long.label");
    const Recorded second = record("test.a", "test.scope.with.a.long.label");
    const Recorded third = record("test.a", "test.scope.with.a.long.label");
    const Recorded fourth = record("test.b", "test.scope.with.a.long.label");
    const Recorded fifth = record("test.b", "test.other.with.a.long.label");
    REQUIRE(third.list == first.list);
    REQUIRE(fifth.list == third.list);
    REQUIRE(fourth.list == second.list);
    // beginCommandBuffer and setLabel when the list is new or its label changed, beginCommandBuffer alone otherwise.
    CHECK(first.acquireCalls == 2);
    CHECK(second.acquireCalls == 2);
    CHECK(third.acquireCalls == 1);
    CHECK(fourth.acquireCalls == 2);
    CHECK(fifth.acquireCalls == 2);
    // The command buffer kept its label across the recordings, through commits and slot reuse.
    CHECK(third.label == "test.a");
    CHECK(fourth.label == "test.b");
    CHECK(fifth.label == "test.b");
    // A scope that begins in the same order with the same label reuses its string; a new label makes a new one.
    CHECK(third.scope == first.scope);
    CHECK(third.scopeText == "test.scope.with.a.long.label");
    CHECK(fifth.scope != third.scope);
    CHECK(fifth.scopeText == "test.other.with.a.long.label");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("encoders carry labels only on a device created for validation or capture, or under the validation layer", "[metal4]")
{
    const bool capture = GENERATE(false, true);
    CAPTURE(capture);
    DeviceDesc desc;
    desc.enableCapture = capture;
    Device device = *createDevice(desc);
    const DeviceImpl& impl = DeviceImpl::of(device);
    // commandPools is set under the validation layer and for any device class not positively the bare driver's.
    const bool labelled = capture || impl.commandPools;
    CHECK(impl.encoderLabels == labelled);
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.encoders");
    const uint64_t before = CommandListImpl::of(list).core.counts.nativeRecordCalls;
    list.copyBuffer(a, 0, b, 0, 16);
    MTL4::ComputeCommandEncoder* encoder = CommandListImpl::of(list).encoders.computeEncoder();
    REQUIRE(encoder != nullptr);
    // computeCommandEncoder, setLabel when labelled, setArgumentTable, then the copy.
    CHECK(CommandListImpl::of(list).core.counts.nativeRecordCalls - before == (labelled ? 4u : 3u));
    {
        const metal4::AutoreleasePool pool;
        NS::String* label = encoder->label();
        if (labelled)
        {
            REQUIRE(label != nullptr);
            CHECK(metal4::toStdString(label) == "test.encoders");
        }
        else
        {
            CHECK((label == nullptr || label->length() == 0));
        }
    }
    const std::array lists{ list };
    endAndSubmit(device, lists);
    device.endFrame();
    destroyDevice(device);
}
