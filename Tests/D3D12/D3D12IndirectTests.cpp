//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12IndirectTests.cpp
/// @brief Tests D3D12 indirect draws: the native calls each records, the draw-record constant around them and what they draw.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12ListState.h"
#include "D3D12Textures.h"
#include "ShaderList.h"
#include "Support/Backend.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The width and height of the target the counting cases render to.
    constexpr uint32_t kSize = 4;

    /// The width and height of the draw oracle's target: it places the triangle of a drawId on pixel `drawId` of these rows.
    constexpr uint32_t kOracleEdge = 256;

    /// Four `uint32` words of one cell AbiVertex stores.
    using Cell = std::array<uint32_t, 4>;

    /// The cell an untouched output holds.
    constexpr Cell kUntouched{ 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu };

    /// AbiVertex's root.
    struct VertexRoot
    {
        Addr output;                  ///< Where the entry stores what it observed, one `uint4` per vertex.
        uint32_t firstVertex;         ///< The first vertex index the draw passes.
        uint32_t verticesPerInstance; ///< The vertices of each instance.
    };

    /// A root block no standard entry reads.
    struct UnreadRoot
    {
        std::array<uint32_t, 4> words; ///< Any value.
    };

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

        /// Returns how many messages were collected, and lists them for the log.
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
    /// Returns a new `edge` by `edge` render target of `format` on `device`.
    Texture makeTarget(Device device, Format format = Format::RGBA8Unorm, uint32_t edge = kSize)
    {
        Result<Texture> created =
            device.createTexture({ .format = format, .extent = { edge, edge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" });
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
    /// Returns the bytes of `values`.
    template<class T> std::vector<std::byte> bytesOf(std::span<const T> values)
    {
        const std::span<const std::byte> bytes = std::as_bytes(values);
        return { bytes.begin(), bytes.end() };
    }

    //==================================================================================================================
    /// Returns a new `IndirectArgs` buffer holding `records`.
    template<class T> Buffer argsBuffer(Device device, std::span<const T> records)
    {
        return makeBuffer(device, { .size = records.size_bytes(), .usage = BufferUsage::IndirectArgs, .label = "test.args" }, bytesOf(records));
    }

    //==================================================================================================================
    /// Returns a new buffer AbiVertex can store `cells` cells into, each holding `kUntouched`.
    Buffer outputBuffer(Device device, uint64_t cells)
    {
        const std::vector<Cell> initial(cells, kUntouched);
        return makeBuffer(
            device, { .size = cells * sizeof(Cell), .usage = BufferUsage::ShaderWrite, .label = "test.output" }, bytesOf(std::span<const Cell>(initial))
        );
    }

    //==================================================================================================================
    /// Returns a new index buffer holding `indices`.
    Buffer indexBuffer(Device device, std::span<const uint32_t> indices)
    {
        return makeBuffer(device, { .size = indices.size_bytes(), .usage = BufferUsage::Index, .label = "test.indices" }, bytesOf(indices));
    }

    //==================================================================================================================
    /// Loads the standard library on `device`.
    ShaderLibrary loadStandard(Device device)
    {
        const Result<ShaderLibrary> library = device.loadShaderLibrary(test::standardLibrary(), "test.standard");
        INFO((library ? std::string() : library.error().message));
        REQUIRE(library.has_value());
        return *library;
    }

    //==================================================================================================================
    /// Loads the packaged conformance library `name` on `device`.
    ShaderLibrary loadLibrary(Device device, std::string_view name)
    {
        const Result<ShaderLibrary> library = device.loadShaderLibrary(test::shaderLibraryFile(name), std::string("test.") + std::string(name));
        INFO((library ? std::string() : library.error().message));
        REQUIRE(library.has_value());
        return *library;
    }

    //==================================================================================================================
    /// Creates `desc` over one color target of `format` on `device`, failing the case with the error's message when creation
    /// fails.
    Pipeline create(Device device, GraphicsPipelineDesc desc, Format format = Format::RGBA8Unorm)
    {
        const std::array colors{ ColorTarget{ .format = format } };
        desc.colors = colors;
        const Result<Pipeline> pipeline = device.createPipeline(desc);
        INFO(desc.label);
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    //==================================================================================================================
    /// Creates the standard vertex and fragment pipeline of `library`.
    Pipeline standardPipeline(Device device, ShaderLibrary library)
    {
        return create(device, GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .label = "test.standard" });
    }

    //==================================================================================================================
    /// Creates the standard mesh pipeline of `library`, without a task stage.
    Pipeline meshPipeline(Device device, ShaderLibrary library)
    {
        return create(device, GraphicsPipelineDesc{ .mesh = { library, "msMain" }, .fragment = { library, "fsMain" }, .label = "test.mesh" });
    }

    //==================================================================================================================
    /// Creates AbiVertex's pipeline on `device`: no fragment entry, so only its stores are observable.
    Pipeline abiVertexPipeline(Device device)
    {
        return create(device, GraphicsPipelineDesc{ .vertex = { loadLibrary(device, "AbiVertex"), "vertexMain" }, .label = "test.abiVertex" });
    }

    //==================================================================================================================
    /// Creates the draw oracle's pipeline on `device` over an `R32Float` target, which holds each drawn drawId plus one.
    Pipeline oraclePipeline(Device device)
    {
        const ShaderLibrary library = loadLibrary(device, "DrawOracle");
        return create(
            device,
            GraphicsPipelineDesc{ .vertex = { library, "oracleVertexMain" }, .fragment = { library, "oracleFragmentMain" }, .label = "test.oracle" },
            Format::R32Float
        );
    }

    //==================================================================================================================
    /// Returns the payload of `buffer`, a live buffer of `device`.
    const d3d12::BufferPayload& bufferOf(Device device, Buffer buffer)
    {
        const auto* payload = static_cast<const d3d12::BufferPayload*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
        REQUIRE(payload != nullptr);
        return *payload;
    }

    //==================================================================================================================
    /// Returns the payload of `texture`, a live texture of `device`.
    const d3d12::TexturePayload& textureOf(Device device, Texture texture)
    {
        const auto* payload = static_cast<const d3d12::TexturePayload*>(DeviceImpl::of(device).core.pool<Texture>().native(texture));
        REQUIRE(payload != nullptr);
        return *payload;
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
    /// Records one barrier on `list` that moves every subresource of `target` from `Undefined` to `ColorAttachment`.
    void toColorAttachment(CommandList list, Texture target)
    {
        const std::array barriers{ TextureBarrier{ .texture = target,
                                                   .dstStage = Stage::ColorAttachment,
                                                   .dstAccess = Access::ColorAttachmentWrite,
                                                   .oldLayout = Layout::Undefined,
                                                   .newLayout = Layout::ColorAttachment } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Moves `target` to `ColorAttachment`, begins a pass over the whole of it, `edge` texels square, that clears it, and
    /// binds `pipeline`, the full viewport and the full scissor.
    void beginPass(CommandList list, Texture target, Pipeline pipeline, uint32_t edge = kSize)
    {
        toColorAttachment(list, target);
        const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, edge, edge }, .label = "test.pass" });
        list.setPipeline(pipeline);
        list.setViewport(Viewport{ .width = static_cast<float>(edge), .height = static_cast<float>(edge) });
        list.setScissor(Rect2D{ 0, 0, edge, edge });
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
    /// Returns the first `cells` cells of `output`, a buffer of `device` no queue is using, read natively.
    std::vector<std::byte> readCells(Device device, Buffer output, uint64_t cells)
    {
        return test::readNativeBuffer(DeviceImpl::of(device), bufferOf(device, output).resource, cells * sizeof(Cell));
    }

    //==================================================================================================================
    /// Returns the cells AbiVertex stores for vertices 0, 1 and 2 of instance 0 of a draw that reads `drawId`.
    std::vector<std::byte> triangleCells(uint32_t drawId)
    {
        const std::array cells{ Cell{ 0, 0, drawId, 0 }, Cell{ 1, 0, drawId, 0 }, Cell{ 2, 0, drawId, 0 } };
        return bytesOf(std::span<const Cell>(cells));
    }
} // namespace

//======================================================================================================================
TEST_CASE("an indirect draw after setRoot costs the root SRV and one ExecuteIndirect, no b1 write", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = standardPipeline(device, loadStandard(device));
    const Texture target = makeTarget(device);
    const std::array records{ DrawIndirectArgs{ .drawId = 5, .vertexCount = 3, .instanceCount = 1 } };
    const Buffer args = argsBuffer(device, std::span<const DrawIndirectArgs>(records));
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.indirect");
    const d3d12::Binding& binding = stateOf(list).binding;
    beginPass(list, target, pipeline);
    (void)list.pushRoot(UnreadRoot{});
    // SetGraphicsRootShaderResourceView and ExecuteIndirect: the command signature writes b1 from each record.
    uint64_t before = recordCalls(list);
    list.drawIndirect(device.address(args), 1);
    CHECK(recordCalls(list) - before == 2u);
    CHECK_FALSE(binding.graphicsRootDirty);
    CHECK(binding.drawIdDirty);
    // ExecuteIndirect alone.
    before = recordCalls(list);
    list.drawIndirect(device.address(args), 1);
    CHECK(recordCalls(list) - before == 1u);
    CHECK(binding.drawIdDirty);
    list.endRenderPass();
    run(device, list, done);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(args);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a direct draw after an indirect draw writes b1 once; a second direct draw writes nothing more", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = abiVertexPipeline(device);
    const Texture target = makeTarget(device);
    constexpr uint32_t kVertices = 3;
    constexpr uint32_t kDrawId = 42;
    const Buffer indirectOutput = outputBuffer(device, kVertices);
    const Buffer directOutput = outputBuffer(device, kVertices);
    // The second record draws no vertex: the direct draws after it share its root, and so its cells, and two draws' stores
    // to one cell are unordered within a pass.
    const std::array records{ DrawIndirectArgs{ .drawId = kDrawId, .vertexCount = kVertices, .instanceCount = 1 },
                              DrawIndirectArgs{ .drawId = 7, .vertexCount = 0, .instanceCount = 1 } };
    const Buffer args = argsBuffer(device, std::span<const DrawIndirectArgs>(records));
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.indirect");
    const d3d12::Binding& binding = stateOf(list).binding;
    beginPass(list, target, pipeline);
    (void)list.pushRoot(VertexRoot{ device.address(indirectOutput), 0, kVertices });
    // The root and ExecuteIndirect; afterwards b1 holds the record's drawId.
    uint64_t before = recordCalls(list);
    list.drawIndirect(device.address(args), 1);
    CHECK(recordCalls(list) - before == 2u);
    CHECK(binding.drawIdDirty);
    // The root and ExecuteIndirect again, for the record that draws nothing.
    (void)list.pushRoot(VertexRoot{ device.address(directOutput), 0, kVertices });
    before = recordCalls(list);
    list.drawIndirect(device.address(args, sizeof(DrawIndirectArgs)), 1);
    CHECK(recordCalls(list) - before == 2u);
    // b1 back to 0 and DrawInstanced, with no root: the signature changes b1 only, so the root survives ExecuteIndirect.
    before = recordCalls(list);
    list.draw(kVertices);
    CHECK(recordCalls(list) - before == 2u);
    CHECK_FALSE(binding.drawIdDirty);
    // DrawInstanced alone: the list holds 0 in b1.
    before = recordCalls(list);
    list.draw(kVertices);
    CHECK(recordCalls(list) - before == 1u);
    list.endRenderPass();
    run(device, list, done);
    CHECK(readCells(device, indirectOutput, kVertices) == triangleCells(kDrawId));
    // Both direct draws store into the cells of the root bound before the second ExecuteIndirect, each with drawId 0.
    CHECK(readCells(device, directOutput, kVertices) == triangleCells(0));
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(indirectOutput);
    device.destroy(directOutput);
    device.destroy(args);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("drawMeshTasksIndirect is one ExecuteIndirect, and the next direct mesh draw writes b1 once", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    REQUIRE(device.capabilities().meshTasksIndirect);
    const MessageCollection messages;
    const Pipeline pipeline = meshPipeline(device, loadStandard(device));
    const Texture target = makeTarget(device);
    const std::array records{ DrawMeshTasksIndirectArgs{ .drawId = 7, .x = 1, .y = 1, .z = 1 } };
    const Buffer args = argsBuffer(device, std::span<const DrawMeshTasksIndirectArgs>(records));
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.meshIndirect");
    const d3d12::Binding& binding = stateOf(list).binding;
    beginPass(list, target, pipeline);
    // b1 and DispatchMesh: a list without a root binds none.
    uint64_t before = recordCalls(list);
    list.drawMeshTasks(1, 1, 1);
    CHECK(recordCalls(list) - before == 2u);
    CHECK_FALSE(binding.drawIdDirty);
    // ExecuteIndirect alone.
    before = recordCalls(list);
    list.drawMeshTasksIndirect(device.address(args), 1);
    CHECK(recordCalls(list) - before == 1u);
    CHECK(binding.drawIdDirty);
    // b1 and DispatchMesh, then DispatchMesh alone.
    before = recordCalls(list);
    list.drawMeshTasks(1, 1, 1);
    CHECK(recordCalls(list) - before == 2u);
    before = recordCalls(list);
    list.drawMeshTasks(1, 1, 1);
    CHECK(recordCalls(list) - before == 1u);
    list.endRenderPass();
    run(device, list, done);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(args);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("drawIndexedIndirect sets the index buffer only when it differs", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = standardPipeline(device, loadStandard(device));
    const Texture target = makeTarget(device);
    // Three 32-bit indices, which read as six 16-bit ones: 12 bytes either way.
    const std::array<uint32_t, 3> values{ 0, 1, 2 };
    const Buffer indices = indexBuffer(device, values);
    const Buffer other = indexBuffer(device, values);
    const std::array records{ DrawIndexedIndirectArgs{ .drawId = 0, .indexCount = 3, .instanceCount = 1 } };
    const Buffer args = argsBuffer(device, std::span<const DrawIndexedIndirectArgs>(records));
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.indexedIndirect");
    const d3d12::DynamicState& dynamic = stateOf(list).dynamic;
    beginPass(list, target, pipeline);
    // IASetIndexBuffer and ExecuteIndirect.
    uint64_t before = recordCalls(list);
    list.drawIndexedIndirect(indices, IndexType::UInt16, device.address(args), 1);
    CHECK(recordCalls(list) - before == 2u);
    CHECK(dynamic.indexBuffer.BufferLocation == bufferOf(device, indices).base);
    CHECK(dynamic.indexBuffer.SizeInBytes == 12u);
    CHECK(dynamic.indexBuffer.Format == DXGI_FORMAT_R16_UINT);
    // The same buffer and type: ExecuteIndirect alone.
    before = recordCalls(list);
    list.drawIndexedIndirect(indices, IndexType::UInt16, device.address(args), 1);
    CHECK(recordCalls(list) - before == 1u);
    // A direct draw shares the view: b1 and DrawIndexedInstanced.
    before = recordCalls(list);
    list.drawIndexed(indices, IndexType::UInt16, 3);
    CHECK(recordCalls(list) - before == 2u);
    // Another type of the same buffer, then another buffer: IASetIndexBuffer and ExecuteIndirect each.
    before = recordCalls(list);
    list.drawIndexedIndirect(indices, IndexType::UInt32, device.address(args), 1);
    CHECK(recordCalls(list) - before == 2u);
    CHECK(dynamic.indexBuffer.Format == DXGI_FORMAT_R32_UINT);
    before = recordCalls(list);
    list.drawIndexedIndirect(other, IndexType::UInt32, device.address(args), 1);
    CHECK(recordCalls(list) - before == 2u);
    CHECK(dynamic.indexBuffer.BufferLocation == bufferOf(device, other).base);
    before = recordCalls(list);
    list.drawIndexedIndirect(other, IndexType::UInt32, device.address(args), 1);
    CHECK(recordCalls(list) - before == 1u);
    list.endRenderPass();
    run(device, list, done);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(indices);
    device.destroy(other);
    device.destroy(args);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("maxCount 0 records nothing", "[d3d12]")
{
#ifndef NDEBUG
    SKIP("a Debug build checks VU-rojo-indirect-0003, which a maxCount of 0 breaks");
#else
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const ShaderLibrary library = loadStandard(device);
    const Pipeline pipeline = standardPipeline(device, library);
    const Pipeline mesh = meshPipeline(device, library);
    const Texture target = makeTarget(device);
    const std::array<uint32_t, 3> values{ 0, 1, 2 };
    const Buffer indices = indexBuffer(device, values);
    const std::array draws{ DrawIndirectArgs{ .drawId = 1, .vertexCount = 3, .instanceCount = 1 } };
    const std::array indexed{ DrawIndexedIndirectArgs{ .drawId = 1, .indexCount = 3, .instanceCount = 1 } };
    const std::array meshes{ DrawMeshTasksIndirectArgs{ .drawId = 1, .x = 1, .y = 1, .z = 1 } };
    const Buffer drawArgs = argsBuffer(device, std::span<const DrawIndirectArgs>(draws));
    const Buffer indexedArgs = argsBuffer(device, std::span<const DrawIndexedIndirectArgs>(indexed));
    const Buffer meshArgs = argsBuffer(device, std::span<const DrawMeshTasksIndirectArgs>(meshes));
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.maxCount");
    const d3d12::ListState& state = stateOf(list);
    beginPass(list, target, pipeline);
    list.draw(3);
    (void)list.pushRoot(UnreadRoot{});
    // No flush, no index buffer and no ExecuteIndirect: the root stays owed and b1 still holds 0.
    const uint64_t before = recordCalls(list);
    list.drawIndirect(device.address(drawArgs), 0);
    list.drawIndexedIndirect(indices, IndexType::UInt32, device.address(indexedArgs), 0);
    list.setPipeline(mesh);
    const uint64_t afterBind = recordCalls(list);
    list.drawMeshTasksIndirect(device.address(meshArgs), 0);
    CHECK(afterBind - before == 1u);
    CHECK(recordCalls(list) == afterBind);
    CHECK(state.binding.graphicsRootDirty);
    CHECK_FALSE(state.binding.drawIdDirty);
    CHECK(state.dynamic.indexBuffer.BufferLocation == 0u);
    list.endRenderPass();
    run(device, list, done);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(indices);
    device.destroy(drawArgs);
    device.destroy(indexedArgs);
    device.destroy(meshArgs);
    device.destroy(done);
    destroyDevice(device);
#endif
}

//======================================================================================================================
TEST_CASE("a count of 0 draws nothing and a count above maxCount draws maxCount", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = oraclePipeline(device);
    const Texture target = makeTarget(device, Format::R32Float, kOracleEdge);
    // Records 0 to 3 draw drawIds 0 to 3, records 4 to 9 drawIds 10 to 15 and records 10 to 14 drawIds 20 to 24, each on
    // its own pixel; one buffer holds them all, so each draw also starts at a byte offset of its own.
    std::vector<DrawIndirectArgs> records;
    for (const auto& [first, count] : { std::array<uint32_t, 2>{ 0, 4 }, std::array<uint32_t, 2>{ 10, 6 }, std::array<uint32_t, 2>{ 20, 5 } })
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            records.push_back(DrawIndirectArgs{ .drawId = first + i, .vertexCount = 3, .instanceCount = 1 });
        }
    }
    const Buffer args = argsBuffer(device, std::span<const DrawIndirectArgs>(records));
    // The indexed records, laid out alike, draw drawIds 30 to 33, 40 to 45 and 50 to 54 through indices 0, 1 and 2.
    std::vector<DrawIndexedIndirectArgs> indexedRecords;
    for (const DrawIndirectArgs& record : records)
    {
        indexedRecords.push_back(DrawIndexedIndirectArgs{ .drawId = record.drawId + 30, .indexCount = 3, .instanceCount = 1 });
    }
    const Buffer indexedArgs = argsBuffer(device, std::span<const DrawIndexedIndirectArgs>(indexedRecords));
    const std::array<uint32_t, 3> values{ 0, 1, 2 };
    const Buffer indices = indexBuffer(device, values);
    // Two counts in one buffer: 0 at byte 0 and 6, above the maxCount of 4, at byte 4.
    const std::array<uint32_t, 2> counts{ 0, 6 };
    const Buffer counted = argsBuffer(device, std::span<const uint32_t>(counts));
    constexpr uint32_t kMaxCount = 4;
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.counts");
    beginPass(list, target, pipeline, kOracleEdge);
    list.drawIndirect(device.address(args), kMaxCount, device.address(counted));
    list.drawIndirect(device.address(args, 4 * sizeof(DrawIndirectArgs)), kMaxCount, device.address(counted, sizeof(uint32_t)));
    // A null count draws exactly maxCount records: drawId 24 stays undrawn.
    list.drawIndirect(device.address(args, 10 * sizeof(DrawIndirectArgs)), kMaxCount);
    // The same counts through indexed draws: drawId 54 stays undrawn.
    list.drawIndexedIndirect(indices, IndexType::UInt32, device.address(indexedArgs), kMaxCount, device.address(counted));
    list.drawIndexedIndirect(
        indices, IndexType::UInt32, device.address(indexedArgs, 4 * sizeof(DrawIndexedIndirectArgs)), kMaxCount, device.address(counted, sizeof(uint32_t))
    );
    list.drawIndexedIndirect(indices, IndexType::UInt32, device.address(indexedArgs, 10 * sizeof(DrawIndexedIndirectArgs)), kMaxCount);
    list.endRenderPass();
    run(device, list, done);
    std::vector<float> expected(size_t{ kOracleEdge } * kOracleEdge, 0.0f);
    for (const uint32_t drawId : { 10u, 11u, 12u, 13u, 20u, 21u, 22u, 23u, 40u, 41u, 42u, 43u, 50u, 51u, 52u, 53u })
    {
        expected[drawId] = static_cast<float>(drawId + 1);
    }
    const std::vector<std::byte> texels =
        test::readNativeTexture(DeviceImpl::of(device), textureOf(device, target).resource, 0, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
    REQUIRE(texels.size() == expected.size() * sizeof(float));
    std::vector<float> observed(expected.size());
    std::memcpy(observed.data(), texels.data(), texels.size());
    // The first row holds every drawId the records name; the rest of the target stays clear.
    CHECK(std::vector<float>(observed.begin(), observed.begin() + 64) == std::vector<float>(expected.begin(), expected.begin() + 64));
    CHECK(observed == expected);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(args);
    device.destroy(indexedArgs);
    device.destroy(indices);
    device.destroy(counted);
    device.destroy(done);
    destroyDevice(device);
}
