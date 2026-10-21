//----------------------------------------------------------------------------------------------------------------------
/// @file DrawTests.cpp
/// @brief Tests roots, draws, dispatches and indirect commands against chapters 05, 07 and 08.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// A root block in the flat layout of chapter 05.
    struct Root
    {
        Addr vertices;
        uint32_t albedo;
        uint32_t drawId;
        uint32_t padding[4];
    };

    constexpr std::array<std::byte, 16> kBytes = {};
} // namespace

//======================================================================================================================
TEST_CASE("setRoot binds an address 256 bytes into a buffer and again after a pass", "[contract]")
{
    PassKit kit;
    const Buffer buffer = kit.buffer(BufferUsage::None, 1024);
    kit.list.setRoot(kit.device.address(buffer));
    kit.list.setRoot(kit.device.address(buffer, 768));
    kit.beginSimplePass();
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("pushRoot returns the address of its copy, which setRoot binds again", "[contract]")
{
    PassKit kit;
    const Root root{ .albedo = 3, .drawId = 9 };
    const Addr first = kit.list.pushRoot(root);
    const Addr second = kit.list.pushRoot(root);
    kit.list.setRoot(first);
    CHECK((first.lo != second.lo || first.hi != second.hi));
    kit.finish();
}

//======================================================================================================================
TEST_CASE("frame data of the open frame is a valid root, in the list that uploaded it and in another", "[contract]")
{
    PassKit kit;
    const Addr data = kit.list.uploadFrameData(kBytes);
    CommandList other = kit.device.acquireCommandList(QueueType::Graphics, "other");
    other.setRoot(data);
    kit.list.setRoot(data);
    other.end();
    kit.list.end();
    const std::array<CommandList, 2> lists{ kit.list, other };
    kit.device.queue(QueueType::Graphics).submit(lists);
    kit.device.endFrame();
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("every direct draw and dispatch records in the state its entry requires", "[contract]")
{
    PassKit kit;
    kit.readyToDraw();
    kit.list.draw(3);
    kit.list.draw(0, 0);
    kit.list.drawIndexed(kit.buffer(BufferUsage::Index, 64), IndexType::UInt16, 32);
    kit.list.drawIndexed(kit.buffer(BufferUsage::Index, 64), IndexType::UInt32, 8, 2, 8, -1, 1);
    kit.list.endRenderPass();
    kit.readyToDraw(true);
    kit.list.drawMeshTasks(1, 1, 1);
    const Limits limits = kit.device.capabilities().limits;
    kit.list.drawMeshTasks(limits.maxMeshTaskGroups, 1, 1);
    kit.list.endRenderPass();
    kit.list.setPipeline(kit.compute());
    kit.list.dispatch(1, 1, 1);
    kit.list.dispatch(limits.maxDispatchGroups, limits.maxDispatchGroups, 0);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("an indexed draw may read the last index of a buffer sized to the byte", "[contract]")
{
    PassKit kit;
    kit.readyToDraw();
    kit.list.drawIndexed(kit.buffer(BufferUsage::Index, 6), IndexType::UInt16, 2, 1, 1);
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("indirect draws take records and an optional count from IndirectArgs buffers", "[contract]")
{
    PassKit kit;
    const Buffer args = kit.buffer(BufferUsage::IndirectArgs, 1024);
    const Addr records = kit.device.address(args);
    const Addr count = kit.device.address(args, 1020);
    kit.readyToDraw();
    kit.list.drawIndirect(records, 10);
    kit.list.drawIndirect(records + 256, 4, count);
    kit.list.drawIndexedIndirect(kit.buffer(BufferUsage::Index), IndexType::UInt32, records, 10, count);
    kit.list.drawIndexedIndirect(kit.buffer(BufferUsage::Index), IndexType::UInt16, records, 1);
    kit.list.endRenderPass();
    kit.readyToDraw(true);
    kit.list.drawMeshTasksIndirect(records, 64, count);
    kit.list.endRenderPass();
    kit.list.setPipeline(kit.compute());
    kit.list.dispatchIndirect(records + 1012);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("frame data serves as the records and count of indirect commands", "[contract]")
{
    PassKit kit;
    const std::array<DrawIndirectArgs, 2> draws{};
    const std::array<uint32_t, 1> drawCount{ 2 };
    const std::array<DispatchIndirectArgs, 1> dispatches{};
    const Addr records = kit.list.uploadFrameData(std::as_bytes(std::span(draws)), 4);
    const Addr count = kit.list.uploadFrameData(std::as_bytes(std::span(drawCount)), 4);
    const Addr groups = kit.list.uploadFrameData(std::as_bytes(std::span(dispatches)), 4);
    kit.readyToDraw();
    kit.list.drawIndirect(records, 2, count);
    kit.list.endRenderPass();
    kit.list.setPipeline(kit.compute());
    kit.list.dispatchIndirect(groups);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("an indirect draw of the largest allowed maxCount names a sufficiently large buffer", "[contract]")
{
    PassKit kit;
    const Limits limits = kit.device.capabilities().limits;
    const uint64_t size = uint64_t{ limits.maxDrawIndirectCount } * sizeof(DrawIndexedIndirectArgs);
    if (size > limits.maxBufferSize)
    {
        // Chapter 15 bounds the count and the buffer size separately, so a device may allow more records than one buffer holds.
        kit.finish();
        SKIP("maxDrawIndirectCount records exceed maxBufferSize, so no buffer holds the largest maxCount");
    }
    const Buffer args = kit.buffer(BufferUsage::IndirectArgs, size);
    kit.readyToDraw();
    kit.list.drawIndirect(kit.device.address(args), limits.maxDrawIndirectCount);
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("pushRoot accepts the roots chapter 05 allows and returns an address", "[contract]")
{
    static_assert(std::is_same_v<decltype(std::declval<CommandList&>().pushRoot(Root{})), Addr>);
    static_assert(std::is_standard_layout_v<Root> && std::is_trivially_copyable_v<Root>);
    struct Largest
    {
        std::array<uint32_t, 16384> words;
    };
    PassKit kit;
    (void)kit.list.pushRoot(Largest{});
    kit.finish();
}
