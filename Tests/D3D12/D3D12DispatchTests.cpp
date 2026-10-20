//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12DispatchTests.cpp
/// @brief Tests D3D12 compute binding and dispatches: the native calls each records and the grid the GPU writes.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12ListState.h"
#include "D3D12PipelineState.h"
#include "ShaderList.h"
#include "Support/Backend.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The dispatch oracle's threadgroup size.
    constexpr uint32_t kGroupX = 8;
    constexpr uint32_t kGroupY = 4;
    constexpr uint32_t kGroupZ = 2;

    /// The bytes of one `uint4` the oracle stores.
    constexpr uint64_t kCell = 16;

    /// The cells of one group's grid.
    constexpr uint64_t kGroupCells = uint64_t{ kGroupX } * kGroupY * kGroupZ;

    /// The dispatch oracle's root.
    struct GridRoot
    {
        Addr output;     ///< Where the entry stores a `uint4` per thread.
        uint32_t width;  ///< The grid's width in threads.
        uint32_t height; ///< The grid's height in threads.
    };

    /// Four `uint32` words of one cell.
    using Cell = std::array<uint32_t, 4>;

    /// The cell an untouched output holds.
    constexpr Cell kUntouched{ 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu };

    /// Guards `g_messages`, which the message callback may fill from any thread.
    std::mutex g_messagesMutex;

    /// The messages at `Warning` or above the library reported while a case collected them.
    std::vector<std::string> g_messages;

    //==================================================================================================================
    /// Keeps `message` when it is a warning or an error. It may run on any thread.
    void collectMessage(MessageSeverity severity, std::string_view message, void*)
    {
        if (severity == MessageSeverity::Info)
        {
            return;
        }
        const std::lock_guard lock(g_messagesMutex);
        g_messages.emplace_back(message);
    }

    /// Collects the library's warnings and errors, the debug layer's included, into `g_messages`, emptied first, while it
    /// lives, and removes the callback when it goes, however the case ends.
    class MessageCollection
    {
      public:
        MessageCollection();
        ~MessageCollection();

        //==============================================================================================================
        MessageCollection(const MessageCollection&) = delete;

        //==============================================================================================================
        MessageCollection& operator=(const MessageCollection&) = delete;

        /// Returns the messages collected so far, and lists them for the log.
        size_t count() const;
    };

    //==================================================================================================================
    MessageCollection::MessageCollection()
    {
        {
            const std::lock_guard lock(g_messagesMutex);
            g_messages.clear();
        }
        setMessageCallback(collectMessage, nullptr);
    }

    //==================================================================================================================
    MessageCollection::~MessageCollection()
    {
        setMessageCallback(nullptr, nullptr);
    }

    //==================================================================================================================
    size_t MessageCollection::count() const
    {
        const std::lock_guard lock(g_messagesMutex);
        for (const std::string& message : g_messages)
        {
            UNSCOPED_INFO("message: " << message);
        }
        return g_messages.size();
    }

    //==================================================================================================================
    /// Returns a new device that keeps two frames in flight.
    Device makeDevice()
    {
        DeviceDesc desc;
        desc.framesInFlight = 2;
        Result<Device> created = createDevice(desc);
        INFO((created ? std::string() : created.error().message));
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new fence at zero on `device`, labelled `label`.
    Fence makeFence(Device device, std::string_view label)
    {
        Result<Fence> created = device.createFence(0, label);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new buffer of `desc` on `device`, holding `initial` when it is not empty.
    Buffer makeBuffer(Device device, const BufferDesc& desc, std::span<const std::byte> initial = {})
    {
        Result<Buffer> created = device.createBuffer(desc, initial);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Creates the dispatch oracle's compute pipeline on `device`.
    Pipeline oraclePipeline(Device device)
    {
        const std::vector<std::byte> blob = test::shaderLibraryFile("DispatchOracle");
        const Result<ShaderLibrary> library = device.loadShaderLibrary(blob, "test.DispatchOracle");
        INFO((library ? std::string() : library.error().message));
        REQUIRE(library.has_value());
        const Result<Pipeline> pipeline = device.createPipeline(ComputePipelineDesc{ .compute = { *library, "gridMain" }, .label = "test.gridMain" });
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    //==================================================================================================================
    /// Returns the payload of `pipeline`, read as recording reads it.
    const d3d12::PipelineState& payloadOf(Device device, Pipeline pipeline)
    {
        const auto* state = static_cast<const d3d12::PipelineState*>(DeviceImpl::of(device).core.pool<Pipeline>().native(pipeline));
        REQUIRE(state != nullptr);
        REQUIRE(state->pso != nullptr);
        return *state;
    }

    //==================================================================================================================
    /// Returns the native buffer of `buffer`, a live buffer of `device`.
    ID3D12Resource* resourceOf(Device device, Buffer buffer)
    {
        const auto* payload = static_cast<const d3d12::BufferPayload*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
        REQUIRE(payload != nullptr);
        return payload->resource;
    }

    //==================================================================================================================
    /// Returns the native state of `list`.
    d3d12::ListState& stateOf(CommandList list)
    {
        return CommandListImpl::of(list).state;
    }

    //==================================================================================================================
    /// Returns the native calls `list` has recorded since its acquisition.
    uint64_t recordCalls(CommandList list)
    {
        return core::coreOf(list).counts.nativeRecordCalls;
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
    /// What the oracle stores for a grid of `groupsX` by `groupsY` by `groupsZ` groups: the thread's identifier and a one at
    /// its linear index, and `rest` elsewhere, `cells` cells in all.
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
    /// Creates a Gpu buffer of `cells` cells, each `kUntouched`, that a shader may write.
    Buffer outputBuffer(Device device, uint64_t cells)
    {
        const std::vector<Cell> initial(cells, kUntouched);
        return makeBuffer(device, { .size = cells * kCell, .usage = BufferUsage::ShaderWrite, .label = "test.output" }, bytesOf(initial));
    }

    //==================================================================================================================
    /// The oracle's root for a grid `groupsX` by `groupsY` groups wide writing to `output`.
    GridRoot rootOf(Device device, Buffer output, uint32_t groupsX, uint32_t groupsY)
    {
        return GridRoot{ device.address(output), groupsX * kGroupX, groupsY * kGroupY };
    }

    //==================================================================================================================
    /// Ends `list`, submits it on the Graphics queue signalling `done` to 1, ends the frame and waits for the signal.
    void run(Device device, CommandList list, Fence done)
    {
        list.end();
        const std::array<FenceValue, 1> signal{ FenceValue{ done, 1 } };
        device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, signal);
        device.endFrame();
        REQUIRE(device.wait({ done, 1 }, UINT64_MAX));
    }

    //==================================================================================================================
    /// Returns the cells of `output`, a buffer of `cells` cells on `device` that no queue is using, read natively.
    std::vector<std::byte> readCells(Device device, Buffer output, uint64_t cells)
    {
        return test::readNativeBuffer(DeviceImpl::of(device), resourceOf(device, output), cells * kCell);
    }
} // namespace

//======================================================================================================================
TEST_CASE("setPipeline with a compute pipeline sets it at once and a dispatch after it costs one call", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = oraclePipeline(device);
    const d3d12::PipelineState& payload = payloadOf(device, pipeline);
    const Buffer output = outputBuffer(device, kGroupCells);
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.dispatch");
    (void)list.pushRoot(rootOf(device, output, 1, 1));
    uint64_t before = recordCalls(list);
    list.setPipeline(pipeline);
    // SetPipelineState, at once.
    CHECK(recordCalls(list) - before == 1u);
    const d3d12::Binding& binding = stateOf(list).binding;
    CHECK(binding.pipeline == &payload);
    CHECK(binding.computePipeline == payload.pso);
    CHECK(binding.graphicsPipeline == nullptr);
    // The first dispatch also binds the root.
    list.dispatch(1, 1, 1);
    before = recordCalls(list);
    list.setPipeline(pipeline);
    // The native list holds the pipeline already.
    CHECK(recordCalls(list) - before == 0u);
    before = recordCalls(list);
    list.dispatch(1, 1, 1);
    // Dispatch alone.
    CHECK(recordCalls(list) - before == 1u);
    run(device, list, done);
    CHECK(readCells(device, output, kGroupCells) == bytesOf(expectedGrid(1, 1, 1, kUntouched, kGroupCells)));
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(output);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("setRoot then dispatch writes the compute root once; a second dispatch writes nothing more", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = oraclePipeline(device);
    const std::array<Buffer, 2> outputs = { outputBuffer(device, kGroupCells), outputBuffer(device, kGroupCells) };
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.root");
    list.setPipeline(pipeline);
    const d3d12::Binding& binding = stateOf(list).binding;
    for (const Buffer output : outputs)
    {
        uint64_t before = recordCalls(list);
        // pushRoot uploads the root and sets it, which records nothing.
        (void)list.pushRoot(rootOf(device, output, 1, 1));
        CHECK(recordCalls(list) - before == 0u);
        CHECK(binding.computeRootDirty);
        before = recordCalls(list);
        list.dispatch(1, 1, 1);
        // SetComputeRootShaderResourceView and Dispatch.
        CHECK(recordCalls(list) - before == 2u);
        CHECK_FALSE(binding.computeRootDirty);
        // The graphics side still owes the root to the next draw.
        CHECK(binding.graphicsRootDirty);
        before = recordCalls(list);
        list.dispatch(1, 1, 1);
        CHECK(recordCalls(list) - before == 1u);
    }
    run(device, list, done);
    for (const Buffer output : outputs)
    {
        CHECK(readCells(device, output, kGroupCells) == bytesOf(expectedGrid(1, 1, 1, kUntouched, kGroupCells)));
    }
    CHECK(messages.count() == 0u);
    device.destroy(done);
    for (const Buffer output : outputs)
    {
        device.destroy(output);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a fill between two dispatches sets the caller's pipeline and root again before the second, once", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    const Pipeline pipeline = oraclePipeline(device);
    const d3d12::PipelineState& payload = payloadOf(device, pipeline);
    const uint64_t cells = 2 * kGroupCells;
    const Buffer output = outputBuffer(device, cells);
    const Fence done = makeFence(device, "test.done");
    // A pattern of unequal bytes runs the backend's fill kernel, which sets its own root signature and pipeline.
    constexpr uint32_t kFill = 0x01020304u;
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.fill");
    list.setPipeline(pipeline);
    (void)list.pushRoot(rootOf(device, output, 1, 1));
    list.dispatch(1, 1, 1);
    list.barrier(
        std::array{
            GlobalBarrier{ .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite } }
    );
    list.fillBuffer(output, 0, cells * kCell, kFill);
    const d3d12::Binding& binding = stateOf(list).binding;
    REQUIRE(impl.kernels.fill() != nullptr);
    CHECK(binding.computeRoot == d3d12::ComputeRoot::Kernel);
    CHECK(binding.computePipeline == impl.kernels.fill());
    CHECK(binding.computeRootDirty);
    CHECK(binding.pipeline == &payload);
    list.barrier(
        std::array{
            GlobalBarrier{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderWrite } }
    );
    uint64_t before = recordCalls(list);
    list.dispatch(1, 1, 1);
    // SetComputeRootSignature, SetPipelineState, SetComputeRootShaderResourceView and Dispatch.
    CHECK(recordCalls(list) - before == 4u);
    CHECK(binding.computeRoot == d3d12::ComputeRoot::Global);
    CHECK(binding.computePipeline == payload.pso);
    CHECK_FALSE(binding.computeRootDirty);
    before = recordCalls(list);
    list.dispatch(1, 1, 1);
    // The restore happened once: Dispatch alone.
    CHECK(recordCalls(list) - before == 1u);
    run(device, list, done);
    // The fill cleared everything, then the dispatches after it wrote the grid again through the same root.
    const Cell filled{ kFill, kFill, kFill, kFill };
    CHECK(readCells(device, output, cells) == bytesOf(expectedGrid(1, 1, 1, filled, cells)));
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(output);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("dispatchIndirect is one ExecuteIndirect after the flush", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = oraclePipeline(device);
    const DispatchIndirectArgs groups{ 2, 3, 1 };
    std::array<std::byte, sizeof(DispatchIndirectArgs)> record = {};
    std::memcpy(record.data(), &groups, record.size());
    const Buffer args = makeBuffer(device, { .size = sizeof(DispatchIndirectArgs), .usage = BufferUsage::IndirectArgs, .label = "test.args" }, record);
    const uint64_t cells = uint64_t{ 2 * kGroupX } * (3 * kGroupY) * kGroupZ;
    const Buffer output = outputBuffer(device, cells + 16);
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.dispatchIndirect");
    list.setPipeline(pipeline);
    (void)list.pushRoot(rootOf(device, output, 2, 3));
    uint64_t before = recordCalls(list);
    list.dispatchIndirect(device.address(args));
    // SetComputeRootShaderResourceView and ExecuteIndirect.
    CHECK(recordCalls(list) - before == 2u);
    before = recordCalls(list);
    list.dispatchIndirect(device.address(args));
    // ExecuteIndirect alone.
    CHECK(recordCalls(list) - before == 1u);
    run(device, list, done);
    // The sixteen cells past the grid keep their value: the record's counts, not more, were dispatched.
    CHECK(readCells(device, output, cells + 16) == bytesOf(expectedGrid(2, 3, 1, kUntouched, cells + 16)));
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(output);
    device.destroy(args);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a dispatch with a zero dimension records nothing", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = oraclePipeline(device);
    const Buffer output = outputBuffer(device, kGroupCells);
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.zero");
    list.setPipeline(pipeline);
    (void)list.pushRoot(rootOf(device, output, 1, 1));
    const d3d12::Binding& binding = stateOf(list).binding;
    for (const std::array<uint32_t, 3> grid : { std::array<uint32_t, 3>{ 0, 1, 1 }, std::array<uint32_t, 3>{ 1, 0, 1 }, std::array<uint32_t, 3>{ 1, 1, 0 } })
    {
        INFO("grid " << grid[0] << " by " << grid[1] << " by " << grid[2]);
        const uint64_t before = recordCalls(list);
        list.dispatch(grid[0], grid[1], grid[2]);
        CHECK(recordCalls(list) - before == 0u);
        // Nothing was flushed: the root still waits for a dispatch that runs.
        CHECK(binding.computeRootDirty);
    }
    run(device, list, done);
    CHECK(readCells(device, output, kGroupCells) == bytesOf(std::vector<Cell>(kGroupCells, kUntouched)));
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(output);
    destroyDevice(device);
}
