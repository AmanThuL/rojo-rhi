//----------------------------------------------------------------------------------------------------------------------
/// @file IndirectHazardDeathTests.cpp
/// @brief Death tests for indirect argument and count reads that no barrier orders after the GPU writes before them.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// The bytes of every argument buffer.
    constexpr uint64_t kBytes = 1024;

    //==================================================================================================================
    /// Creates an `IndirectArgs` buffer of `kBytes` bytes that a fill may write.
    Buffer argsBuffer(PassKit& kit)
    {
        return kit.buffer(BufferUsage::IndirectArgs | BufferUsage::ShaderWrite, kBytes);
    }

    //==================================================================================================================
    /// Records one global barrier from `srcStage` and `srcAccess` to `dstStage` and `dstAccess`.
    void global(CommandList list, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
    {
        const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{
            .srcStage = srcStage, .srcAccess = srcAccess, .dstStage = dstStage, .dstAccess = dstAccess } };
        list.barrier(globals);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0005 drawIndirect reads records a fill wrote with no barrier between", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer args = argsBuffer(kit);
        kit.list.fillBuffer(args, 0, kBytes, 0);
        kit.readyToDraw();
        kit.list.drawIndirect(kit.device.address(args), 1);
        kit.list.endRenderPass();
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0005 drawIndirect reads a count a fill wrote with no barrier between", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer args = argsBuffer(kit);
        const Buffer count = argsBuffer(kit);
        kit.list.fillBuffer(count, 0, 4, 1);
        kit.readyToDraw();
        kit.list.drawIndirect(kit.device.address(args), 1, kit.device.address(count));
        kit.list.endRenderPass();
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0011 drawIndexedIndirect reads records after a barrier whose destination is not IndirectArgs", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0011", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer args = argsBuffer(kit);
        const Buffer indices = kit.buffer(BufferUsage::Index);
        kit.list.fillBuffer(args, 0, kBytes, 0);
        global(kit.list, Stage::Copy, Access::CopyWrite, Stage::VertexShader, Access::ShaderRead);
        kit.readyToDraw();
        kit.list.drawIndexedIndirect(indices, IndexType::UInt32, kit.device.address(args), 1);
        kit.list.endRenderPass();
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0011 drawIndexedIndirect reads a count a fill wrote with no barrier between", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0011", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer args = argsBuffer(kit);
        const Buffer count = argsBuffer(kit);
        const Buffer indices = kit.buffer(BufferUsage::Index);
        kit.list.fillBuffer(count, 0, 4, 1);
        kit.readyToDraw();
        kit.list.drawIndexedIndirect(indices, IndexType::UInt32, kit.device.address(args), 1, kit.device.address(count));
        kit.list.endRenderPass();
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0017 drawMeshTasksIndirect reads records a fill wrote with no barrier between", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0017", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer args = argsBuffer(kit);
        kit.list.fillBuffer(args, 0, kBytes, 0);
        kit.readyToDraw(true);
        kit.list.drawMeshTasksIndirect(kit.device.address(args), 1);
        kit.list.endRenderPass();
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0017 drawMeshTasksIndirect reads a count a fill wrote with no barrier between", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0017", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer args = argsBuffer(kit);
        const Buffer count = argsBuffer(kit);
        kit.list.fillBuffer(count, 0, 4, 1);
        kit.readyToDraw(true);
        kit.list.drawMeshTasksIndirect(kit.device.address(args), 1, kit.device.address(count));
        kit.list.endRenderPass();
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0020 dispatchIndirect reads a record a fill wrote with no barrier between", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0020", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer args = argsBuffer(kit);
        kit.list.fillBuffer(args, 0, kBytes, 0);
        kit.list.setPipeline(kit.compute());
        kit.list.dispatchIndirect(kit.device.address(args));
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 an indirect read follows a barrier to IndirectArgs whose source misses the write", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer args = argsBuffer(kit);
        kit.list.fillBuffer(args, 0, kBytes, 0);
        global(kit.list, Stage::ComputeShader, Access::ShaderWrite, Stage::IndirectArgs, Access::IndirectArgs);
        kit.list.setPipeline(kit.compute());
        kit.list.dispatchIndirect(kit.device.address(args));
        kit.finish();
    });
}
