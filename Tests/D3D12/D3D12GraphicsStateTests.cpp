//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12GraphicsStateTests.cpp
/// @brief Tests D3D12 graphics binding, dynamic state and direct draws: the native calls each records and what the GPU writes.
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
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The width and height of the target the cases render to.
    constexpr uint32_t kSize = 4;

    /// The one color target of every pipeline the cases create.
    constexpr std::array<ColorTarget, 1> kColor = { ColorTarget{ .format = Format::RGBA8Unorm } };

    /// The viewport and scissor of the whole target.
    constexpr Viewport kFull{ .width = static_cast<float>(kSize), .height = static_cast<float>(kSize) };
    constexpr Rect2D kFullRect{ 0, 0, kSize, kSize };

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
    /// Returns a new `kSize` by `kSize` RGBA8 render target on `device`.
    Texture makeTarget(Device device)
    {
        Result<Texture> created =
            device.createTexture({ .format = Format::RGBA8Unorm, .extent = { kSize, kSize, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" });
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
    /// Returns the bytes of `cells`.
    std::vector<std::byte> bytesOf(std::span<const Cell> cells)
    {
        std::vector<std::byte> bytes(cells.size() * sizeof(Cell));
        std::memcpy(bytes.data(), cells.data(), bytes.size());
        return bytes;
    }

    //==================================================================================================================
    /// Returns a new buffer AbiVertex can store `cells` cells into, each holding `kUntouched`.
    Buffer outputBuffer(Device device, uint64_t cells)
    {
        const std::vector<Cell> initial(cells, kUntouched);
        return makeBuffer(device, { .size = cells * sizeof(Cell), .usage = BufferUsage::ShaderWrite, .label = "test.output" }, bytesOf(initial));
    }

    //==================================================================================================================
    /// Returns a new index buffer holding `bytes`.
    Buffer indexBuffer(Device device, std::span<const std::byte> bytes)
    {
        return makeBuffer(device, { .size = bytes.size(), .usage = BufferUsage::Index, .label = "test.indices" }, bytes);
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
    /// Creates `desc` over `kColor` on `device`, failing the case with the error's message when creation fails.
    Pipeline create(Device device, GraphicsPipelineDesc desc)
    {
        desc.colors = kColor;
        const Result<Pipeline> pipeline = device.createPipeline(desc);
        INFO(desc.label);
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    //==================================================================================================================
    /// Creates the standard vertex and fragment pipeline of `library`, with `raster`.
    Pipeline standardPipeline(Device device, ShaderLibrary library, const RasterState& raster = {})
    {
        return create(
            device, GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .raster = raster, .label = "test.standard" }
        );
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
    /// Returns the payload of `pipeline`, read as recording reads it.
    const d3d12::PipelineState& payloadOf(Device device, Pipeline pipeline)
    {
        const auto* state = static_cast<const d3d12::PipelineState*>(DeviceImpl::of(device).core.pool<Pipeline>().native(pipeline));
        REQUIRE(state != nullptr);
        REQUIRE(state->pso != nullptr);
        return *state;
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
    /// Returns the barrier between two passes that write `target`, which stays in `ColorAttachment`.
    TextureBarrier betweenPasses(Texture target)
    {
        return { .texture = target,
                 .srcStage = Stage::ColorAttachment,
                 .srcAccess = Access::ColorAttachmentWrite,
                 .dstStage = Stage::ColorAttachment,
                 .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                 .oldLayout = Layout::ColorAttachment,
                 .newLayout = Layout::ColorAttachment };
    }

    //==================================================================================================================
    /// Returns a pass over `colors` that renders to the whole target.
    RenderPassDesc passOver(const std::array<ColorAttachment, 1>& colors)
    {
        return RenderPassDesc{ .colors = colors, .area = kFullRect, .label = "test.pass" };
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
} // namespace

//======================================================================================================================
TEST_CASE("the first draw after setRoot costs the root, b1 and the draw; the next draw costs one call", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = standardPipeline(device, loadStandard(device));
    const Texture target = makeTarget(device);
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.draw");
    const d3d12::Binding& binding = stateOf(list).binding;
    // The draw-record constant is unset after acquisition.
    CHECK(binding.drawIdDirty);
    toColorAttachment(list, target);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOver(colors));
    list.setPipeline(pipeline);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    const Addr root = list.pushRoot(UnreadRoot{});
    CHECK(binding.root == d3d12::rootAddress(DeviceImpl::of(device).directory, root));
    CHECK(binding.graphicsRootDirty);
    // SetGraphicsRootShaderResourceView, SetGraphicsRoot32BitConstant and DrawInstanced.
    uint64_t before = recordCalls(list);
    list.draw(3);
    CHECK(recordCalls(list) - before == 3u);
    CHECK_FALSE(binding.graphicsRootDirty);
    CHECK_FALSE(binding.drawIdDirty);
    // The compute side still owes the root to the next dispatch.
    CHECK(binding.computeRootDirty);
    before = recordCalls(list);
    list.draw(3, 2);
    CHECK(recordCalls(list) - before == 1u);
    // A root set again costs one call at the next draw, and b1 stays.
    (void)list.pushRoot(UnreadRoot{ { 1, 2, 3, 4 } });
    before = recordCalls(list);
    list.draw(3);
    CHECK(recordCalls(list) - before == 2u);
    list.endRenderPass();
    run(device, list, done);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("setPipeline sets PSO and topology once; the same pipeline again sends nothing", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = standardPipeline(device, loadStandard(device));
    const d3d12::PipelineState& payload = payloadOf(device, pipeline);
    const Texture target = makeTarget(device);
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.bind");
    const d3d12::ListState& state = stateOf(list);
    // The topology the native list holds is unknown after acquisition.
    CHECK(state.dynamic.topology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
    toColorAttachment(list, target);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOver(colors));
    // SetPipelineState and IASetPrimitiveTopology.
    uint64_t before = recordCalls(list);
    list.setPipeline(pipeline);
    CHECK(recordCalls(list) - before == 2u);
    CHECK(state.binding.pipeline == &payload);
    CHECK(state.binding.graphicsPipeline == payload.pso);
    CHECK(state.binding.computePipeline == nullptr);
    CHECK(state.dynamic.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    before = recordCalls(list);
    list.setPipeline(pipeline);
    CHECK(recordCalls(list) - before == 0u);
    // A strip, then the pipeline again: chapter 06 resets the topology to the class's list form, one call.
    list.setPrimitiveTopology(PrimitiveTopology::TriangleStrip);
    CHECK(state.dynamic.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    before = recordCalls(list);
    list.setPipeline(pipeline);
    CHECK(recordCalls(list) - before == 1u);
    CHECK(state.dynamic.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // The pass's end unbinds the caller's pipeline, as the core does; the native list still holds it.
    list.endRenderPass();
    CHECK(state.binding.pipeline == nullptr);
    CHECK(state.binding.graphicsPipeline == payload.pso);
    // BeginRenderPass changes no pipeline and no topology, so the next pass binds the same pipeline with no call.
    list.barrier({}, {}, std::array{ betweenPasses(target) });
    list.beginRenderPass(passOver(colors));
    CHECK(state.binding.pipeline == nullptr);
    before = recordCalls(list);
    list.setPipeline(pipeline);
    CHECK(recordCalls(list) - before == 0u);
    CHECK(state.binding.pipeline == &payload);
    list.endRenderPass();
    run(device, list, done);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a mesh pipeline sets no topology", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const ShaderLibrary library = loadStandard(device);
    const Pipeline vertex = standardPipeline(device, library);
    const Pipeline mesh = meshPipeline(device, library);
    const d3d12::PipelineState& payload = payloadOf(device, mesh);
    REQUIRE(payload.kind == d3d12::PipelineKind::Mesh);
    const Texture target = makeTarget(device);
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.mesh");
    const d3d12::ListState& state = stateOf(list);
    toColorAttachment(list, target);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOver(colors));
    // SetPipelineState alone.
    uint64_t before = recordCalls(list);
    list.setPipeline(mesh);
    CHECK(recordCalls(list) - before == 1u);
    CHECK(state.binding.graphicsPipeline == payload.pso);
    CHECK(state.dynamic.topology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    // b1 and DispatchMesh: a list without a root binds none.
    before = recordCalls(list);
    list.drawMeshTasks(1, 1, 1);
    CHECK(recordCalls(list) - before == 2u);
    // A vertex pipeline after it sets its PSO and its topology; the mesh pipeline after that leaves the topology held.
    before = recordCalls(list);
    list.setPipeline(vertex);
    CHECK(recordCalls(list) - before == 2u);
    before = recordCalls(list);
    list.setPipeline(mesh);
    CHECK(recordCalls(list) - before == 1u);
    CHECK(state.dynamic.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    before = recordCalls(list);
    list.drawMeshTasks(1, 1, 1);
    CHECK(recordCalls(list) - before == 1u);
    list.endRenderPass();
    run(device, list, done);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("equal dynamic state sends no call; a changed value sends one", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = standardPipeline(device, loadStandard(device));
    const Texture target = makeTarget(device);
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.dynamic");
    const d3d12::DynamicState& dynamic = stateOf(list).dynamic;
    // The viewport and scissor the native list holds are unknown after acquisition.
    CHECK_FALSE(dynamic.viewport.has_value());
    CHECK_FALSE(dynamic.scissor.has_value());
    toColorAttachment(list, target);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOver(colors));
    list.setPipeline(pipeline);
    // Each setter: the first value sends one call, the same value none, another value one.
    uint64_t before = recordCalls(list);
    list.setViewport(kFull);
    CHECK(recordCalls(list) - before == 1u);
    REQUIRE(dynamic.viewport.has_value());
    CHECK(dynamic.viewport->Width == static_cast<float>(kSize));
    CHECK(dynamic.viewport->MaxDepth == 1.0f);
    list.setViewport(kFull);
    CHECK(recordCalls(list) - before == 1u);
    list.setViewport(Viewport{ .x = 1.0f, .y = 1.0f, .width = 2.0f, .height = 2.0f, .minDepth = 0.25f, .maxDepth = 0.75f });
    CHECK(recordCalls(list) - before == 2u);
    CHECK(dynamic.viewport->TopLeftX == 1.0f);
    CHECK(dynamic.viewport->MinDepth == 0.25f);
    before = recordCalls(list);
    list.setScissor(Rect2D{ 1, 2, 2, 1 });
    CHECK(recordCalls(list) - before == 1u);
    REQUIRE(dynamic.scissor.has_value());
    // A scissor's right and bottom edges are exclusive coordinates.
    CHECK(dynamic.scissor->left == 1);
    CHECK(dynamic.scissor->top == 2);
    CHECK(dynamic.scissor->right == 3);
    CHECK(dynamic.scissor->bottom == 3);
    list.setScissor(Rect2D{ 1, 2, 2, 1 });
    CHECK(recordCalls(list) - before == 1u);
    list.setScissor(kFullRect);
    CHECK(recordCalls(list) - before == 2u);
    // The pass began with stencil reference 0 and blend constants 0, so those values send nothing.
    before = recordCalls(list);
    list.setStencilReference(0);
    list.setBlendConstants(0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(recordCalls(list) - before == 0u);
    list.setStencilReference(3);
    list.setStencilReference(3);
    CHECK(recordCalls(list) - before == 1u);
    CHECK(dynamic.stencilReference == uint8_t{ 3 });
    list.setBlendConstants(0.5f, 0.25f, 2.0f, 0.5f);
    list.setBlendConstants(0.5f, 0.25f, 2.0f, 0.5f);
    CHECK(recordCalls(list) - before == 2u);
    CHECK(dynamic.blendFactor == std::array{ 0.5f, 0.25f, 2.0f, 0.5f });
    // setPipeline left the triangle list.
    before = recordCalls(list);
    list.setPrimitiveTopology(PrimitiveTopology::TriangleList);
    CHECK(recordCalls(list) - before == 0u);
    list.setPrimitiveTopology(PrimitiveTopology::TriangleStrip);
    list.setPrimitiveTopology(PrimitiveTopology::TriangleStrip);
    CHECK(recordCalls(list) - before == 1u);
    list.endRenderPass();
    // The native list keeps its viewport and scissor across passes; the pass writes its stencil reference and blend
    // constants back to 0, two calls.
    list.barrier({}, {}, std::array{ betweenPasses(target) });
    before = recordCalls(list);
    list.beginRenderPass(passOver(colors));
    CHECK(recordCalls(list) - before == 5u);
    list.setPipeline(pipeline);
    before = recordCalls(list);
    list.setViewport(Viewport{ .x = 1.0f, .y = 1.0f, .width = 2.0f, .height = 2.0f, .minDepth = 0.25f, .maxDepth = 0.75f });
    list.setScissor(kFullRect);
    list.setStencilReference(0);
    CHECK(recordCalls(list) - before == 0u);
    list.draw(3);
    list.endRenderPass();
    run(device, list, done);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a depthBias pipeline re-issues the recorded bias at every setPipeline; a pipeline without depthBias sends none", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const ShaderLibrary library = loadStandard(device);
    const Pipeline biased = standardPipeline(device, library, RasterState{ .depthBias = true });
    const Pipeline plain = standardPipeline(device, library);
    REQUIRE(payloadOf(device, biased).depthBias);
    REQUIRE_FALSE(payloadOf(device, plain).depthBias);
    const Texture target = makeTarget(device);
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.bias");
    const d3d12::DynamicState& dynamic = stateOf(list).dynamic;
    toColorAttachment(list, target);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOver(colors));
    // SetPipelineState, IASetPrimitiveTopology and RSSetDepthBias with the pass's zero bias.
    uint64_t before = recordCalls(list);
    list.setPipeline(biased);
    CHECK(recordCalls(list) - before == 3u);
    // Recorded and sent once; the same values again send nothing.
    before = recordCalls(list);
    list.setDepthBias(4.0f, 0.5f, 0.125f);
    CHECK(recordCalls(list) - before == 1u);
    CHECK(dynamic.depthBias == std::array{ 4.0f, 0.5f, 0.125f });
    list.setDepthBias(4.0f, 0.5f, 0.125f);
    CHECK(recordCalls(list) - before == 1u);
    // The same pipeline again: its PSO and topology are held, its bias is sent again.
    before = recordCalls(list);
    list.setPipeline(biased);
    CHECK(recordCalls(list) - before == 1u);
    // A pipeline without depthBias: its PSO alone, the topology being the same list form.
    before = recordCalls(list);
    list.setPipeline(plain);
    CHECK(recordCalls(list) - before == 1u);
    CHECK(dynamic.depthBias == std::array{ 4.0f, 0.5f, 0.125f });
    // Back to the biased pipeline: its PSO and the recorded bias.
    before = recordCalls(list);
    list.setPipeline(biased);
    CHECK(recordCalls(list) - before == 2u);
    list.endRenderPass();
    // The next pass records a zero bias without a call, and the held pipeline sends it at its bind.
    list.barrier({}, {}, std::array{ betweenPasses(target) });
    list.beginRenderPass(passOver(colors));
    CHECK(dynamic.depthBias == std::array<float, 3>{});
    before = recordCalls(list);
    list.setPipeline(biased);
    CHECK(recordCalls(list) - before == 1u);
    list.endRenderPass();
    run(device, list, done);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("drawIndexed sets the index buffer once per buffer and format, with firstIndex as StartIndexLocation", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = abiVertexPipeline(device);
    const Pipeline standard = standardPipeline(device, loadStandard(device));
    const Texture target = makeTarget(device);
    constexpr uint32_t kVertices = 3;
    const Buffer output = outputBuffer(device, kVertices);
    // From index 1 the 16-bit indices name the vertices 0, 1 and 2; the index skipped would store past the output.
    const std::array<uint16_t, 5> narrow{ 9, 0, 1, 2, 0 };
    const Buffer indices = indexBuffer(device, std::as_bytes(std::span(narrow)));
    const d3d12::BufferPayload& payload = bufferOf(device, indices);
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.indexed");
    const d3d12::DynamicState& dynamic = stateOf(list).dynamic;
    CHECK(dynamic.indexBuffer.BufferLocation == 0u);
    toColorAttachment(list, target);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOver(colors));
    list.setPipeline(pipeline);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    (void)list.pushRoot(VertexRoot{ device.address(output), 0, kVertices });
    // The root, b1, IASetIndexBuffer and DrawIndexedInstanced.
    uint64_t before = recordCalls(list);
    list.drawIndexed(indices, IndexType::UInt16, kVertices, 1, 1);
    CHECK(recordCalls(list) - before == 4u);
    CHECK(dynamic.indexBuffer.BufferLocation == payload.base);
    CHECK(dynamic.indexBuffer.SizeInBytes == 10u);
    CHECK(dynamic.indexBuffer.Format == DXGI_FORMAT_R16_UINT);
    // The same buffer and format: the draw alone.
    before = recordCalls(list);
    list.drawIndexed(indices, IndexType::UInt16, kVertices, 1, 1);
    CHECK(recordCalls(list) - before == 1u);
    // The same buffer as 32-bit indices, drawn through the standard pipeline, which stores nothing: its whole 4-byte
    // indices, 8 of its 10 bytes.
    list.setPipeline(standard);
    before = recordCalls(list);
    list.drawIndexed(indices, IndexType::UInt32, 0);
    CHECK(recordCalls(list) - before == 0u);
    list.drawIndexed(indices, IndexType::UInt32, 1);
    CHECK(recordCalls(list) - before == 2u);
    CHECK(dynamic.indexBuffer.SizeInBytes == 8u);
    CHECK(dynamic.indexBuffer.Format == DXGI_FORMAT_R32_UINT);
    list.endRenderPass();
    run(device, list, done);
    // Vertices 0, 1 and 2 of instance 0, drawId 0, each in its own cell.
    const std::vector<Cell> expected{ Cell{ 0, 0, 0, 0 }, Cell{ 1, 0, 0, 0 }, Cell{ 2, 0, 0, 0 } };
    CHECK(readCells(device, output, kVertices) == bytesOf(expected));
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(output);
    device.destroy(indices);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a zero-count draw, indexed draw or mesh draw records nothing", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const ShaderLibrary library = loadStandard(device);
    const Pipeline vertex = standardPipeline(device, library);
    const Pipeline mesh = meshPipeline(device, library);
    const Texture target = makeTarget(device);
    const std::array<uint16_t, 4> narrow{ 0, 1, 2, 0 };
    const Buffer indices = indexBuffer(device, std::as_bytes(std::span(narrow)));
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.empty");
    const d3d12::ListState& state = stateOf(list);
    toColorAttachment(list, target);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOver(colors));
    list.setPipeline(vertex);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    (void)list.pushRoot(UnreadRoot{});
    uint64_t before = recordCalls(list);
    list.draw(0);
    list.draw(3, 0);
    list.drawIndexed(indices, IndexType::UInt16, 0);
    list.drawIndexed(indices, IndexType::UInt16, 3, 0);
    CHECK(recordCalls(list) - before == 0u);
    list.setPipeline(mesh);
    before = recordCalls(list);
    list.drawMeshTasks(0, 1, 1);
    list.drawMeshTasks(1, 0, 1);
    list.drawMeshTasks(1, 1, 0);
    CHECK(recordCalls(list) - before == 0u);
    // No flush, no index buffer and no draw was recorded.
    CHECK(state.binding.graphicsRootDirty);
    CHECK(state.binding.drawIdDirty);
    CHECK(state.dynamic.indexBuffer.BufferLocation == 0u);
    list.endRenderPass();
    run(device, list, done);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(indices);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("setRoot before the pass, a fill, then the pass: the first draw binds the caller's root", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = abiVertexPipeline(device);
    const d3d12::PipelineState& payload = payloadOf(device, pipeline);
    const Texture target = makeTarget(device);
    constexpr uint32_t kVertices = 3;
    const Buffer output = outputBuffer(device, kVertices);
    const Buffer scratch = makeBuffer(device, { .size = 64, .usage = BufferUsage::ShaderWrite, .label = "test.scratch" });
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.fill");
    const d3d12::Binding& binding = stateOf(list).binding;
    const Addr root = list.pushRoot(VertexRoot{ device.address(output), 0, kVertices });
    // A pattern of unequal bytes runs the backend's fill kernel, which sets its own root signature and pipeline.
    list.fillBuffer(scratch, 0, 64, 0x01020304u);
    CHECK(binding.computeRoot == d3d12::ComputeRoot::Kernel);
    CHECK(binding.graphicsPipeline == nullptr);
    // The kernel touches the compute side alone: the graphics side still owes the caller's root.
    CHECK(binding.graphicsRootDirty);
    CHECK(binding.root == d3d12::rootAddress(DeviceImpl::of(device).directory, root));
    toColorAttachment(list, target);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(passOver(colors));
    list.setPipeline(pipeline);
    CHECK(binding.graphicsPipeline == payload.pso);
    CHECK(binding.computePipeline == nullptr);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    // The caller's root, b1 and DrawInstanced.
    const uint64_t before = recordCalls(list);
    list.draw(kVertices);
    CHECK(recordCalls(list) - before == 3u);
    list.endRenderPass();
    run(device, list, done);
    // Each vertex stored what it saw through the caller's root: its index, instance 0 and drawId 0.
    const std::vector<Cell> expected{ Cell{ 0, 0, 0, 0 }, Cell{ 1, 0, 0, 0 }, Cell{ 2, 0, 0, 0 } };
    CHECK(readCells(device, output, kVertices) == bytesOf(expected));
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(output);
    device.destroy(scratch);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("C8's sequence costs the native minimum + 1", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    const MessageCollection messages;
    const Pipeline pipeline = standardPipeline(device, loadStandard(device));
    const Texture target = makeTarget(device);
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.c8");
    toColorAttachment(list, target);
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    // The calls each entry of one repetition records: barrier (none before the first pass), beginRenderPass, setPipeline,
    // setViewport, setScissor, draw and endRenderPass.
    using Counts = std::array<uint64_t, 7>;
    std::array<Counts, 3> repetitions{};
    for (size_t repetition = 0; repetition < repetitions.size(); ++repetition)
    {
        Counts& counts = repetitions[repetition];
        uint64_t before = recordCalls(list);
        if (repetition > 0)
        {
            list.barrier({}, {}, std::array{ betweenPasses(target) });
        }
        counts[0] = recordCalls(list) - before;
        before = recordCalls(list);
        list.beginRenderPass(passOver(colors));
        counts[1] = recordCalls(list) - before;
        before = recordCalls(list);
        list.setPipeline(pipeline);
        counts[2] = recordCalls(list) - before;
        before = recordCalls(list);
        list.setViewport(kFull);
        counts[3] = recordCalls(list) - before;
        before = recordCalls(list);
        list.setScissor(kFullRect);
        counts[4] = recordCalls(list) - before;
        before = recordCalls(list);
        list.draw(3);
        counts[5] = recordCalls(list) - before;
        before = recordCalls(list);
        list.endRenderPass();
        counts[6] = recordCalls(list) - before;
    }
    // The first pass: BeginEvent, EndQuery, BeginRenderPass and the two starting values; SetPipelineState and
    // IASetPrimitiveTopology; RSSetViewports; RSSetScissorRects; b1 and DrawInstanced; EndRenderPass, EndQuery, EndEvent.
    CHECK(repetitions[0] == Counts{ 0, 5, 2, 1, 1, 2, 3 });
    // Every later pass: Barrier; BeginEvent, EndQuery, BeginRenderPass; the pipeline, viewport and scissor held; DrawInstanced;
    // EndRenderPass, EndQuery, EndEvent. Eight calls, each entry at its native minimum.
    CHECK(repetitions[1] == Counts{ 1, 3, 0, 0, 0, 1, 3 });
    CHECK(repetitions[2] == repetitions[1]);
    run(device, list, done);
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(done);
    destroyDevice(device);
}
