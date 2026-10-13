//----------------------------------------------------------------------------------------------------------------------
/// @file IndirectHazardTests.cpp
/// @brief Tests that the validation tracker accepts indirect reads ordered after GPU writes, and reads of CPU-written arguments.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// The bytes of every argument buffer.
    [[maybe_unused]] constexpr uint64_t kBytes = 1024;
    /// The offset of the count in an argument buffer, after the records.
    [[maybe_unused]] constexpr uint64_t kCountOffset = 1020;

    //==================================================================================================================
    /// Creates an `IndirectArgs` buffer of `kBytes` bytes that a fill or a shader may write.
    [[maybe_unused]] Buffer argsBuffer(PassKit& kit)
    {
        return kit.buffer(BufferUsage::IndirectArgs | BufferUsage::ShaderWrite, kBytes);
    }

    //==================================================================================================================
    /// Records one global barrier from `srcStage` and `srcAccess` to `dstStage` and `dstAccess`.
    [[maybe_unused]] void global(CommandList list, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
    {
        const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{
            .srcStage = srcStage, .srcAccess = srcAccess, .dstStage = dstStage, .dstAccess = dstAccess } };
        list.barrier(globals);
    }
} // namespace

//======================================================================================================================
TEST_CASE("an indirect dispatch reads a record a compute dispatch wrote, after a barrier to IndirectArgs", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer args = argsBuffer(kit);
    kit.list.setPipeline(kit.compute());
    const std::array<ResourceAccess, 1> writes{ ResourceAccess{ .buffer = args, .stage = Stage::ComputeShader, .access = Access::ShaderWrite } };
    kit.list.declareAccess(writes);
    kit.list.dispatch(1, 1, 1);
    const std::array<BufferBarrier, 1> barriers{ BufferBarrier{ .buffer = args,
                                                                .srcStage = Stage::ComputeShader,
                                                                .srcAccess = Access::ShaderWrite,
                                                                .dstStage = Stage::IndirectArgs,
                                                                .dstAccess = Access::IndirectArgs } };
    kit.list.barrier({}, barriers);
    kit.list.dispatchIndirect(kit.device.address(args));
    kit.list.dispatchIndirect(kit.device.address(args));
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("indirect draws read records and a count a copy wrote, after a global barrier to IndirectArgs", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer args = argsBuffer(kit);
    const Buffer source = kit.buffer(BufferUsage::None, kBytes);
    const Buffer indices = kit.buffer(BufferUsage::Index);
    kit.list.copyBuffer(source, 0, args, 0, kBytes);
    global(kit.list, Stage::Copy, Access::CopyWrite, Stage::IndirectArgs, Access::IndirectArgs);
    const Addr records = kit.device.address(args);
    const Addr count = kit.device.address(args, kCountOffset);
    kit.readyToDraw();
    kit.list.drawIndirect(records, 4, count);
    kit.list.drawIndexedIndirect(indices, IndexType::UInt32, records, 4, count);
    kit.list.endRenderPass();
    kit.readyToDraw(true);
    kit.list.drawMeshTasksIndirect(records, 4, count);
    kit.list.endRenderPass();
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("indirect commands read arguments only the CPU wrote with no barrier", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const std::array<std::byte, kBytes> initial{};
    const Buffer args = *kit.device.createBuffer(BufferDesc{ .size = kBytes, .usage = BufferUsage::IndirectArgs, .label = "test.args" }, initial);
    const Buffer indices = kit.buffer(BufferUsage::Index);
    const Addr records = kit.device.address(args);
    const Addr count = kit.device.address(args, kCountOffset);
    kit.readyToDraw();
    kit.list.drawIndirect(records, 4, count);
    kit.list.drawIndexedIndirect(indices, IndexType::UInt32, records, 4, count);
    kit.list.endRenderPass();
    kit.readyToDraw(true);
    kit.list.drawMeshTasksIndirect(records, 4, count);
    kit.list.endRenderPass();
    kit.list.setPipeline(kit.compute());
    kit.list.dispatchIndirect(records);
    kit.finish();
#endif
}
