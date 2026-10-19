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
