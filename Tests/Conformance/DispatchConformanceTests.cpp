//----------------------------------------------------------------------------------------------------------------------
/// @file DispatchConformanceTests.cpp
/// @brief Observes `dispatch` and `dispatchIndirect` on a GPU through the dispatch oracle.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The oracle's threadgroup size.
    constexpr uint32_t kGroupX = 8;
    constexpr uint32_t kGroupY = 4;
    constexpr uint32_t kGroupZ = 2;

    /// The bytes of one `uint4` the oracle stores.
    constexpr uint64_t kCell = 16;

    /// The oracle's root.
    struct GridRoot
    {
        Addr output;     ///< Where the entry stores a `uint4` per thread.
        uint32_t width;  ///< The grid's width in threads.
        uint32_t height; ///< The grid's height in threads.
    };

    /// Four `uint32` words of one cell.
    using Cell = std::array<uint32_t, 4>;

    //==================================================================================================================
    /// Creates the oracle's compute pipeline on `device`.
    Pipeline oraclePipeline(Device device)
    {
        const std::vector<std::byte> blob = test::shaderLibraryFile("DispatchOracle");
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "DispatchOracle");
        return *device.createPipeline(ComputePipelineDesc{ .compute = { library, "gridMain" }, .label = "test.gridMain" });
    }

    //==================================================================================================================
    /// The bytes of `cells`.
    std::vector<std::byte> bytesOf(std::span<const Cell> cells)
    {
        std::vector<std::byte> bytes(cells.size() * kCell);
        std::memcpy(bytes.data(), cells.data(), bytes.size());
        return bytes;
    }

    //==================================================================================================================
    /// What the oracle stores for a grid of `groups` groups: the thread's identifier and a one at its linear index, and the
    /// cells of `rest` elsewhere, `cells` cells in all.
    std::vector<Cell> expectedGrid(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ, const Cell& rest, uint64_t cells)
    {
        const uint32_t width = groupsX * kGroupX;
        const uint32_t height = groupsY * kGroupY;
        std::vector<Cell> grid(cells, rest);
        for (uint32_t z = 0; z < groupsZ * kGroupZ; ++z)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                for (uint32_t x = 0; x < width; ++x)
                {
                    grid[(uint64_t{ z } * height + y) * width + x] = Cell{ x, y, z, 1 };
                }
            }
        }
        return grid;
    }

    //==================================================================================================================
    /// Creates a Gpu buffer of `cells` cells, each `fill`, that a shader may write.
    Buffer outputBuffer(test::GpuKit& kit, uint64_t cells, const Cell& fill)
    {
        const std::vector<Cell> initial(cells, fill);
        return kit.buffer(cells * kCell, Memory::Gpu, BufferUsage::ShaderWrite, bytesOf(initial));
    }

    //==================================================================================================================
    /// The grid root for `groups` groups writing to `output`.
    GridRoot rootOf(Device device, Buffer output, uint32_t groupsX, uint32_t groupsY)
    {
        return GridRoot{ device.address(output), groupsX * kGroupX, groupsY * kGroupY };
    }

    /// The cell an untouched output holds.
    constexpr Cell kUntouched{ 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu };
} // namespace

//======================================================================================================================
TEST_CASE("dispatch covers groups times the library's threadgroup size, every thread once", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Pipeline pipeline = oraclePipeline(kit.device);
    const uint64_t cells = uint64_t{ 3 * kGroupX } * (2 * kGroupY) * (2 * kGroupZ);
    const Buffer output = outputBuffer(kit, cells + 16, kUntouched);
    CommandList list = kit.list(QueueType::Graphics, "test.dispatch");
    list.setPipeline(pipeline);
    list.pushRoot(rootOf(kit.device, output, 3, 2));
    list.dispatch(3, 2, 2);
    kit.run(std::array{ list });
    // The sixteen cells past the grid keep their value: no thread of another group wrote there.
    CHECK(kit.readBuffer(output) == bytesOf(expectedGrid(3, 2, 2, kUntouched, cells + 16)));
}

//======================================================================================================================
TEST_CASE("dispatchIndirect reads its group counts from a GPU-written record", "[conformance]")
{
    test::requireGpu();
    // One variant binds the pipeline before the copy's barrier, which leaves the barrier on an encoder that holds no work.
    const bool bindFirst = GENERATE(false, true);
    const bool zero = GENERATE(false, true);
    CAPTURE(bindFirst, zero);
    test::GpuKit kit;
    const Pipeline pipeline = oraclePipeline(kit.device);
    const std::array<uint32_t, 3> groups = zero ? std::array<uint32_t, 3>{ 0, 0, 0 } : std::array<uint32_t, 3>{ 2, 3, 1 };
    std::vector<std::byte> record(sizeof(groups));
    std::memcpy(record.data(), groups.data(), record.size());
    const Buffer source = kit.buffer(sizeof(groups), Memory::Gpu, BufferUsage::None, record);
    const Buffer args = kit.buffer(sizeof(groups), Memory::Gpu, BufferUsage::IndirectArgs);
    const uint64_t cells = uint64_t{ 2 * kGroupX } * (3 * kGroupY) * kGroupZ;
    const Buffer output = outputBuffer(kit, cells + 16, kUntouched);
    CommandList list = kit.list(QueueType::Graphics, "test.dispatchIndirect");
    if (bindFirst)
    {
        list.setPipeline(pipeline);
    }
    list.copyBuffer(source, 0, args, 0, sizeof(groups));
    list.barrier(
        std::array{
            GlobalBarrier{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::IndirectArgs, .dstAccess = Access::IndirectArgs } }
    );
    if (!bindFirst)
    {
        list.setPipeline(pipeline);
    }
    list.pushRoot(rootOf(kit.device, output, 2, 3));
    list.dispatchIndirect(kit.device.address(args));
    kit.run(std::array{ list });
    const std::vector<Cell> expected = zero ? std::vector<Cell>(cells + 16, kUntouched) : expectedGrid(2, 3, 1, kUntouched, cells + 16);
    CHECK(kit.readBuffer(output) == bytesOf(expected));
}

//======================================================================================================================
TEST_CASE("a root changed between dispatches reaches each its own", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Pipeline pipeline = oraclePipeline(kit.device);
    const uint64_t cells = uint64_t{ kGroupX } * kGroupY * kGroupZ;
    const Buffer first = outputBuffer(kit, cells, kUntouched);
    const Buffer second = outputBuffer(kit, cells, kUntouched);
    CommandList list = kit.list(QueueType::Graphics, "test.roots");
    list.setPipeline(pipeline);
    list.pushRoot(rootOf(kit.device, first, 1, 1));
    list.dispatch(1, 1, 1);
    list.pushRoot(rootOf(kit.device, second, 1, 1));
    list.dispatch(1, 1, 1);
    kit.run(std::array{ list });
    const std::vector<std::byte> expected = bytesOf(expectedGrid(1, 1, 1, kUntouched, cells));
    CHECK(kit.readBuffer(first) == expected);
    CHECK(kit.readBuffer(second) == expected);
}

//======================================================================================================================
TEST_CASE("a fill between two dispatches leaves the second with the caller's pipeline and root", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Pipeline pipeline = oraclePipeline(kit.device);
    const uint64_t grid = uint64_t{ kGroupX } * kGroupY * kGroupZ;
    const uint64_t cells = 2 * grid;
    const Buffer output = outputBuffer(kit, cells, kUntouched);
    // A pattern of unequal bytes runs the backend's fill kernel, which sets its own pipeline and table on the encoder.
    constexpr uint32_t kFill = 0x01020304u;
    CommandList list = kit.list(QueueType::Graphics, "test.fill");
    list.setPipeline(pipeline);
    list.pushRoot(rootOf(kit.device, output, 1, 1));
    list.dispatch(1, 1, 1);
    list.barrier(
        std::array{
            GlobalBarrier{ .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite } }
    );
    list.fillBuffer(output, 0, cells * kCell, kFill);
    list.barrier(
        std::array{
            GlobalBarrier{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderWrite } }
    );
    list.dispatch(1, 1, 1);
    kit.run(std::array{ list });
    // The fill cleared everything, then the second dispatch wrote its grid again through the same root.
    const Cell filled{ kFill, kFill, kFill, kFill };
    CHECK(kit.readBuffer(output) == bytesOf(expectedGrid(1, 1, 1, filled, cells)));
}

//======================================================================================================================
TEST_CASE("dispatches record on Compute lists as on Graphics lists", "[conformance]")
{
    test::requireGpu();
    const QueueType type = GENERATE(QueueType::Graphics, QueueType::Compute);
    CAPTURE(static_cast<int>(type));
    test::GpuKit kit;
    const Pipeline pipeline = oraclePipeline(kit.device);
    const uint64_t cells = uint64_t{ 2 * kGroupX } * kGroupY * kGroupZ;
    const Buffer output = outputBuffer(kit, cells, kUntouched);
    CommandList list = kit.list(type, "test.queueType");
    list.setPipeline(pipeline);
    list.pushRoot(rootOf(kit.device, output, 2, 1));
    list.dispatch(2, 1, 1);
    kit.run(std::array{ list });
    CHECK(kit.readBuffer(output) == bytesOf(expectedGrid(2, 1, 1, kUntouched, cells)));
}
