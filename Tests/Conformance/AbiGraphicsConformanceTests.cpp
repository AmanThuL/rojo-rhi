//----------------------------------------------------------------------------------------------------------------------
/// @file AbiGraphicsConformanceTests.cpp
/// @brief Runs the vertex and fragment halves of the shader ABI conformance shaders on a GPU and checks what they observed.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// Loads the conformance library `name` on `device`.
    ShaderLibrary loadLibrary(Device device, std::string_view name)
    {
        const std::vector<std::byte> blob = test::shaderLibraryFile(name);
        return *device.loadShaderLibrary(blob, name);
    }

    //==================================================================================================================
    /// The half-precision bits of `value`, which is zero or a normal half-precision value held exactly.
    uint16_t halfOf(float value)
    {
        if (value == 0.0f)
        {
            return 0;
        }
        const auto bits = std::bit_cast<uint32_t>(value);
        const uint32_t sign = (bits >> 16) & 0x8000u;
        const auto exponent = static_cast<uint32_t>(static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15);
        return static_cast<uint16_t>(sign | (exponent << 10) | ((bits >> 13) & 0x3FFu));
    }

    /// AbiVertex's root.
    struct VertexRoot
    {
        Addr output;                  ///< Where the entry stores what it observed, one `uint4` per vertex.
        uint32_t firstVertex;         ///< The first vertex index the draw passes.
        uint32_t verticesPerInstance; ///< The vertices of each instance.
    };

    /// AbiFragment's root.
    struct FragmentRoot
    {
        uint32_t view;    ///< The sampled view.
        uint32_t sampler; ///< The sampler.
        uint32_t pad0;    ///< Padding.
        uint32_t pad1;    ///< Padding.
    };
} // namespace

//======================================================================================================================
TEST_CASE("AbiVertex stores vertexIndex, a zero-based instanceIndex and drawId 0 for draw and drawIndexed", "[conformance]")
{
    test::requireGpu();
    const bool indexed = GENERATE(false, true);
    CAPTURE(indexed);
    constexpr uint32_t kFirstVertex = 5;
    constexpr uint32_t kVertices = 3;
    constexpr uint32_t kInstances = 2;
    test::GpuKit kit;
    const ShaderLibrary library = loadLibrary(kit.device, "AbiVertex");
    // No fragment entry and no depth format: the pipeline does not rasterize, so only the stores are observable.
    const std::array targets{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Pipeline pipeline =
        *kit.device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vertexMain" }, .colors = targets, .label = "test.abiVertex" });
    const Texture target = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
    );
    constexpr uint64_t kOutputBytes = uint64_t{ kVertices } * kInstances * 4 * sizeof(uint32_t);
    const Buffer output = kit.buffer(kOutputBytes, Memory::Gpu, BufferUsage::ShaderWrite);
    // Read from index 1, the indices name the vertices 7, 5 and 6 once baseVertex is added.
    const std::array<uint16_t, 4> indices{ 9, 2, 0, 1 };
    const Buffer indexBuffer = kit.buffer(sizeof(indices), Memory::Gpu, BufferUsage::Index, std::as_bytes(std::span(indices)));
    CommandList list = kit.list(QueueType::Graphics, "test.abiVertex");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 4, 4 }, .label = "test.pass" });
    list.setPipeline(pipeline);
    list.setViewport(Viewport{ .width = 4.0f, .height = 4.0f });
    list.setScissor(Rect2D{ 0, 0, 4, 4 });
    list.pushRoot(VertexRoot{ kit.device.address(output), kFirstVertex, kVertices });
    if (indexed)
    {
        list.drawIndexed(indexBuffer, IndexType::UInt16, kVertices, kInstances, 1, static_cast<int32_t>(kFirstVertex), 7);
    }
    else
    {
        list.draw(kVertices, kInstances, kFirstVertex, 7);
    }
    list.endRenderPass();
    kit.run(std::array{ list });
    const std::vector<std::byte> bytes = kit.readBuffer(output);
    REQUIRE(bytes.size() == kOutputBytes);
    std::vector<uint32_t> observed(bytes.size() / sizeof(uint32_t));
    std::memcpy(observed.data(), bytes.data(), bytes.size());
    std::vector<uint32_t> expected;
    for (uint32_t instance = 0; instance < kInstances; ++instance)
    {
        for (uint32_t vertex = kFirstVertex; vertex < kFirstVertex + kVertices; ++vertex)
        {
            expected.insert(expected.end(), { vertex, instance, 0u, 0u });
        }
    }
    CHECK(observed == expected);
}

//======================================================================================================================
TEST_CASE("AbiFragment samples a view and adds drawId 0 through the oracle vertex", "[conformance]")
{
    test::requireGpu();
    constexpr uint32_t kEdge = 256;
    test::GpuKit kit;
    const ShaderLibrary oracle = loadLibrary(kit.device, "DrawOracle");
    const ShaderLibrary fragment = loadLibrary(kit.device, "AbiFragment");
    const std::array targets{ ColorTarget{ .format = Format::RGBA16Float } };
    const Pipeline pipeline = *kit.device.createPipeline(
        GraphicsPipelineDesc{
            .vertex = { oracle, "oracleVertexMain" }, .fragment = { fragment, "fragmentMain" }, .colors = targets, .label = "test.abiFragment" }
    );
    const std::array<uint16_t, 4> texel{ halfOf(0.25f), halfOf(0.5f), halfOf(0.75f), halfOf(1.0f) };
    const std::array data{ TextureData{ std::as_bytes(std::span(texel)), sizeof(texel), 0 } };
    const Texture sampled = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA16Float, .extent = { 1, 1, 1 }, .usage = TextureUsage::Sampled, .label = "test.texel" }, data
    );
    const ViewIndex view = *kit.device.createView(sampled, {});
    const SamplerIndex sampler =
        *kit.device.createSampler(SamplerDesc{ .filter = SamplerFilter::Nearest, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge });
    const Texture target = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA16Float, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
    );
    CommandList list = kit.list(QueueType::Graphics, "test.abiFragment");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kEdge, kEdge }, .label = "test.pass" });
    list.setPipeline(pipeline);
    list.setViewport(Viewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) });
    list.setScissor(Rect2D{ 0, 0, kEdge, kEdge });
    list.pushRoot(FragmentRoot{ static_cast<uint32_t>(view), static_cast<uint32_t>(sampler), 0, 0 });
    // Two instances: the oracle places them 64 rows apart, both with drawId 0.
    list.draw(3, 2);
    list.endRenderPass();
    kit.run(std::array{ list });
    const std::vector<std::byte> bytes = kit.readTexture(target, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, Format::RGBA16Float);
    std::vector<uint16_t> observed(bytes.size() / sizeof(uint16_t));
    std::memcpy(observed.data(), bytes.data(), bytes.size());
    std::vector<uint16_t> expected(size_t{ kEdge } * kEdge * 4, 0);
    for (const uint32_t row : { 0u, 64u })
    {
        std::memcpy(&expected[size_t{ row } * kEdge * 4], texel.data(), sizeof(texel));
    }
    uint32_t wrong = 0;
    for (size_t i = 0; i < expected.size(); ++i)
    {
        wrong += observed[i] != expected[i] ? 1 : 0;
    }
    CHECK(observed[0] == texel[0]);
    CHECK(observed[size_t{ 64 } * kEdge * 4] == texel[0]);
    CHECK(wrong == 0);
}

//======================================================================================================================
TEST_CASE("AbiVertex reads the record's drawId through drawIndirect, firstInstance invisible", "[conformance]")
{
    test::requireGpu();
    const bool indexed = GENERATE(false, true);
    // A maxCount of 1 runs the writer kernel and a native indirect draw; above 1, a count of 1 runs the indirect command
    // buffer path.
    const uint32_t maxCount = GENERATE(1u, 4u);
    CAPTURE(indexed, maxCount);
    constexpr uint32_t kFirstVertex = 5;
    constexpr uint32_t kVertices = 3;
    constexpr uint32_t kInstances = 2;
    constexpr uint32_t kDrawId = 42;
    test::GpuKit kit;
    const ShaderLibrary library = loadLibrary(kit.device, "AbiVertex");
    const std::array targets{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Pipeline pipeline =
        *kit.device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vertexMain" }, .colors = targets, .label = "test.abiVertex" });
    const Texture target = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
    );
    constexpr uint64_t kOutputBytes = uint64_t{ kVertices } * kInstances * 4 * sizeof(uint32_t);
    const Buffer output = kit.buffer(kOutputBytes, Memory::Gpu, BufferUsage::ShaderWrite);
    // Read from index 1 with baseVertex -5, the indices name the vertices 7, 5 and 6.
    const std::array<uint16_t, 4> indices{ 0, 12, 10, 11 };
    const Buffer indexBuffer = kit.buffer(sizeof(indices), Memory::Gpu, BufferUsage::Index, std::as_bytes(std::span(indices)));
    // Every record is the same draw; the count stops after the first.
    std::vector<std::byte> records;
    for (uint32_t i = 0; i < maxCount; ++i)
    {
        if (indexed)
        {
            const DrawIndexedIndirectArgs record{
                .drawId = kDrawId, .indexCount = kVertices, .instanceCount = kInstances, .firstIndex = 1, .baseVertex = -5, .firstInstance = 7
            };
            const auto* first = reinterpret_cast<const std::byte*>(&record);
            records.insert(records.end(), first, first + sizeof(record));
        }
        else
        {
            const DrawIndirectArgs record{
                .drawId = kDrawId, .vertexCount = kVertices, .instanceCount = kInstances, .firstVertex = kFirstVertex, .firstInstance = 7
            };
            const auto* first = reinterpret_cast<const std::byte*>(&record);
            records.insert(records.end(), first, first + sizeof(record));
        }
    }
    const Buffer args = kit.buffer(records.size(), Memory::Gpu, BufferUsage::IndirectArgs, records);
    const uint32_t one = 1;
    const Buffer count = kit.buffer(sizeof(one), Memory::Gpu, BufferUsage::IndirectArgs, std::as_bytes(std::span(&one, 1)));
    const Addr countAddress = maxCount == 1 ? Addr{} : kit.device.address(count);
    CommandList list = kit.list(QueueType::Graphics, "test.abiVertex");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 4, 4 }, .label = "test.pass" });
    list.setPipeline(pipeline);
    list.setViewport(Viewport{ .width = 4.0f, .height = 4.0f });
    list.setScissor(Rect2D{ 0, 0, 4, 4 });
    list.pushRoot(VertexRoot{ kit.device.address(output), kFirstVertex, kVertices });
    if (indexed)
    {
        list.drawIndexedIndirect(indexBuffer, IndexType::UInt16, kit.device.address(args), maxCount, countAddress);
    }
    else
    {
        list.drawIndirect(kit.device.address(args), maxCount, countAddress);
    }
    list.endRenderPass();
    kit.run(std::array{ list });
    const std::vector<std::byte> bytes = kit.readBuffer(output);
    REQUIRE(bytes.size() == kOutputBytes);
    std::vector<uint32_t> observed(bytes.size() / sizeof(uint32_t));
    std::memcpy(observed.data(), bytes.data(), bytes.size());
    std::vector<uint32_t> expected;
    for (uint32_t instance = 0; instance < kInstances; ++instance)
    {
        for (uint32_t vertex = kFirstVertex; vertex < kFirstVertex + kVertices; ++vertex)
        {
            expected.insert(expected.end(), { vertex, instance, kDrawId, 0u });
        }
    }
    CHECK(observed == expected);
}

//======================================================================================================================
TEST_CASE("AbiFragment adds the record's drawId", "[conformance]")
{
    test::requireGpu();
    const uint32_t maxCount = GENERATE(1u, 4u);
    CAPTURE(maxCount);
    constexpr uint32_t kEdge = 256;
    constexpr uint32_t kDrawId = 3;
    test::GpuKit kit;
    const ShaderLibrary oracle = loadLibrary(kit.device, "DrawOracle");
    const ShaderLibrary fragment = loadLibrary(kit.device, "AbiFragment");
    const std::array targets{ ColorTarget{ .format = Format::RGBA16Float } };
    const Pipeline pipeline = *kit.device.createPipeline(
        GraphicsPipelineDesc{
            .vertex = { oracle, "oracleVertexMain" }, .fragment = { fragment, "fragmentMain" }, .colors = targets, .label = "test.abiFragment" }
    );
    const std::array<uint16_t, 4> texel{ halfOf(0.25f), halfOf(0.5f), halfOf(0.75f), halfOf(1.0f) };
    const std::array data{ TextureData{ std::as_bytes(std::span(texel)), sizeof(texel), 0 } };
    const Texture sampled = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA16Float, .extent = { 1, 1, 1 }, .usage = TextureUsage::Sampled, .label = "test.texel" }, data
    );
    const ViewIndex view = *kit.device.createView(sampled, {});
    const SamplerIndex sampler =
        *kit.device.createSampler(SamplerDesc{ .filter = SamplerFilter::Nearest, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge });
    const Texture target = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA16Float, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
    );
    // Two instances of one record: the oracle places them 64 rows apart in the drawId's column.
    const std::vector<DrawIndirectArgs> records(maxCount, DrawIndirectArgs{ .drawId = kDrawId, .vertexCount = 3, .instanceCount = 2, .firstInstance = 9 });
    const Buffer args = kit.buffer(records.size() * sizeof(DrawIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, std::as_bytes(std::span(records)));
    const uint32_t one = 1;
    const Buffer count = kit.buffer(sizeof(one), Memory::Gpu, BufferUsage::IndirectArgs, std::as_bytes(std::span(&one, 1)));
    CommandList list = kit.list(QueueType::Graphics, "test.abiFragment");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kEdge, kEdge }, .label = "test.pass" });
    list.setPipeline(pipeline);
    list.setViewport(Viewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) });
    list.setScissor(Rect2D{ 0, 0, kEdge, kEdge });
    list.pushRoot(FragmentRoot{ static_cast<uint32_t>(view), static_cast<uint32_t>(sampler), 0, 0 });
    list.drawIndirect(kit.device.address(args), maxCount, maxCount == 1 ? Addr{} : kit.device.address(count));
    list.endRenderPass();
    kit.run(std::array{ list });
    const std::vector<std::byte> bytes = kit.readTexture(target, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, Format::RGBA16Float);
    std::vector<uint16_t> observed(bytes.size() / sizeof(uint16_t));
    std::memcpy(observed.data(), bytes.data(), bytes.size());
    std::vector<uint16_t> expected(size_t{ kEdge } * kEdge * 4, 0);
    const std::array<uint16_t, 4> drawn{ halfOf(0.25f + static_cast<float>(kDrawId)), texel[1], texel[2], texel[3] };
    for (const uint32_t row : { 0u, 64u })
    {
        std::memcpy(&expected[(size_t{ row } * kEdge + kDrawId) * 4], drawn.data(), sizeof(drawn));
    }
    uint32_t wrong = 0;
    for (size_t i = 0; i < expected.size(); ++i)
    {
        wrong += observed[i] != expected[i] ? 1 : 0;
    }
    CHECK(observed[size_t{ kDrawId } * 4] == drawn[0]);
    CHECK(observed[(size_t{ 64 } * kEdge + kDrawId) * 4] == drawn[0]);
    CHECK(wrong == 0);
}

//======================================================================================================================
TEST_CASE("AbiMesh runs its task and mesh entries directly and indirectly under the validation layer", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    if (!kit.device.capabilities().meshTasksIndirect)
    {
        SKIP("the device does not report meshTasksIndirect, so drawMeshTasksIndirect cannot be recorded");
    }
    const ShaderLibrary library = loadLibrary(kit.device, "AbiMesh");
    // No fragment entry and no depth format: rasterization is off, so the entries store nothing observable, and the case
    // checks that every form runs to completion with no validation error.
    const std::array targets{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Pipeline pipeline = *kit.device.createPipeline(
        GraphicsPipelineDesc{ .task = { library, "taskMain" }, .mesh = { library, "meshMain" }, .colors = targets, .label = "test.abiMesh" }
    );
    const Texture target = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
    );
    const std::array<DrawMeshTasksIndirectArgs, 4> records{
        DrawMeshTasksIndirectArgs{ .drawId = 3, .x = 1, .y = 1, .z = 1 },
        DrawMeshTasksIndirectArgs{ .drawId = 5, .x = 2, .y = 1, .z = 1 },
        DrawMeshTasksIndirectArgs{ .drawId = 7, .x = 1, .y = 2, .z = 1 },
        DrawMeshTasksIndirectArgs{ .drawId = 9, .x = 1, .y = 1, .z = 2 },
    };
    const Buffer args = kit.buffer(sizeof(records), Memory::Gpu, BufferUsage::IndirectArgs, std::as_bytes(std::span(records)));
    const uint32_t three = 3;
    const Buffer count = kit.buffer(sizeof(three), Memory::Gpu, BufferUsage::IndirectArgs, std::as_bytes(std::span(&three, 1)));
    CommandList list = kit.list(QueueType::Graphics, "test.abiMesh");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 4, 4 }, .label = "test.pass" });
    list.setPipeline(pipeline);
    list.setViewport(Viewport{ .width = 4.0f, .height = 4.0f });
    list.setScissor(Rect2D{ 0, 0, 4, 4 });
    // A batch and the writer kernel ahead of the render encoder, then a direct draw, a lone record, and a batch that splits the
    // pass.
    list.drawMeshTasksIndirect(kit.device.address(args), 4, kit.device.address(count));
    list.drawMeshTasksIndirect(kit.device.address(args), 1, kit.device.address(count));
    list.drawMeshTasks(2, 1, 1);
    list.drawMeshTasksIndirect(kit.device.address(args, sizeof(DrawMeshTasksIndirectArgs)), 1);
    list.drawMeshTasksIndirect(kit.device.address(args), 4);
    list.endRenderPass();
    list.end();
    const Fence fence = *kit.device.createFence(0, "test.abiMesh");
    kit.device.queue(QueueType::Graphics).submit(std::array{ list }, {}, std::array{ FenceValue{ fence, 1 } });
    CHECK(kit.device.wait({ fence, 1 }, UINT64_MAX));
}
