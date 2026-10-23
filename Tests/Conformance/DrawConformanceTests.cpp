//----------------------------------------------------------------------------------------------------------------------
/// @file DrawConformanceTests.cpp
/// @brief Observes direct draws and dynamic state on a GPU through the draw and mesh oracles.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The edge of the oracle targets, in pixels.
    constexpr uint32_t kEdge = 256;

    /// The rows one instance or task group advances the oracle's pixel by: 16,384 cells of a 256-pixel row.
    constexpr uint32_t kInstanceRows = 64;

    /// The viewport and scissor of the whole target.
    constexpr Viewport kFull{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) };
    constexpr Rect2D kFullRect{ 0, 0, kEdge, kEdge };

    /// The solid fragment entry's color.
    constexpr std::array<float, 4> kSolid{ 0.25f, 0.5f, 0.75f, 1.0f };

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

    /// An `RGBA16Float` image of the oracle target, as half-precision bits in texel order.
    struct Image
    {
        std::vector<uint16_t> texels = std::vector<uint16_t>(size_t{ kEdge } * kEdge * 4, 0); ///< Four halves per texel.

        //==============================================================================================================
        /// Sets the texel at `x`, `y` to `value`.
        void set(uint32_t x, uint32_t y, const std::array<float, 4>& value)
        {
            for (size_t channel = 0; channel < 4; ++channel)
            {
                texels[(size_t{ y } * kEdge + x) * 4 + channel] = halfOf(value[channel]);
            }
        }
    };

    //==================================================================================================================
    /// Describes the texels where `actual` differs from `expected`, the first eight of them; empty when they are equal.
    std::string differences(const Image& actual, const Image& expected)
    {
        std::string text;
        uint32_t found = 0;
        for (size_t texel = 0; texel < expected.texels.size() / 4; ++texel)
        {
            const size_t at = texel * 4;
            if (std::memcmp(&actual.texels[at], &expected.texels[at], 4 * sizeof(uint16_t)) == 0)
            {
                continue;
            }
            if (++found <= 8)
            {
                text += std::format(
                    "({}, {}): {:04x} {:04x} {:04x} {:04x}, expected {:04x} {:04x} {:04x} {:04x}\n",
                    texel % kEdge,
                    texel / kEdge,
                    actual.texels[at],
                    actual.texels[at + 1],
                    actual.texels[at + 2],
                    actual.texels[at + 3],
                    expected.texels[at],
                    expected.texels[at + 1],
                    expected.texels[at + 2],
                    expected.texels[at + 3]
                );
            }
        }
        if (found > 0)
        {
            text += std::format("{} texels differ", found);
        }
        return text;
    }

    //==================================================================================================================
    /// Loads the conformance library `name` on `device`.
    ShaderLibrary loadLibrary(Device device, std::string_view name)
    {
        const std::vector<std::byte> blob = test::shaderLibraryFile(name);
        return *device.loadShaderLibrary(blob, name);
    }

    //==================================================================================================================
    /// Creates the graphics pipeline `desc` describes over the one color target `target`.
    Pipeline pipelineOf(Device device, GraphicsPipelineDesc desc, const ColorTarget& target = ColorTarget{ .format = Format::RGBA16Float })
    {
        const std::array targets{ target };
        desc.colors = targets;
        desc.label = "test.pipeline";
        Result<Pipeline> pipeline = device.createPipeline(desc);
        if (!pipeline)
        {
            FAIL(pipeline.error().message);
        }
        return *pipeline;
    }

    //==================================================================================================================
    /// Creates a `kEdge` square texture of `format` with `usage`.
    Texture targetOf(Device device, Format format = Format::RGBA16Float, TextureUsage usage = TextureUsage::RenderTarget)
    {
        return *device.createTexture(
            TextureDesc{ .type = TextureType::Tex2D, .format = format, .extent = { kEdge, kEdge, 1 }, .usage = usage, .label = "test.target" }
        );
    }

    //==================================================================================================================
    /// A pass over `colors` and `depth` that renders to the whole target.
    RenderPassDesc passOf(std::span<const ColorAttachment> colors, const DepthAttachment& depth = {})
    {
        return RenderPassDesc{ .colors = colors, .depth = depth, .area = kFullRect, .label = "test.pass" };
    }

    //==================================================================================================================
    /// Reads `texture`, an `RGBA16Float` oracle target, back.
    Image readImage(test::GpuKit& kit, Texture texture)
    {
        const std::vector<std::byte> bytes = kit.readTexture(texture, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, Format::RGBA16Float);
        Image image;
        REQUIRE(bytes.size() == image.texels.size() * sizeof(uint16_t));
        std::memcpy(image.texels.data(), bytes.data(), bytes.size());
        return image;
    }

    //==================================================================================================================
    /// Creates an index buffer holding `indices` as `type`.
    Buffer indexBuffer(test::GpuKit& kit, IndexType type, std::span<const uint32_t> indices)
    {
        std::vector<std::byte> bytes;
        for (const uint32_t index : indices)
        {
            if (type == IndexType::UInt16)
            {
                const auto narrow = static_cast<uint16_t>(index);
                const auto* first = reinterpret_cast<const std::byte*>(&narrow);
                bytes.insert(bytes.end(), first, first + sizeof(narrow));
            }
            else
            {
                const auto* first = reinterpret_cast<const std::byte*>(&index);
                bytes.insert(bytes.end(), first, first + sizeof(index));
            }
        }
        return kit.buffer(bytes.size(), Memory::Gpu, BufferUsage::Index, bytes);
    }

    //==================================================================================================================
    /// A viewport over the whole target moved `x` pixels right and `y` pixels down, so the oracle's pixel moves with it.
    Viewport shifted(uint32_t x, uint32_t y)
    {
        return Viewport{ .x = static_cast<float>(x), .y = static_cast<float>(y), .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) };
    }

    //==================================================================================================================
    /// The oracle's value for a triangle of draw identifier 0, instance `instance` and first vertex `base`.
    std::array<float, 4> oracleValue(uint32_t instance, uint32_t base)
    {
        return { 1.0f, static_cast<float>(instance + 1), static_cast<float>(base + 1), 1.0f };
    }

    /// The oracle pipelines a case needs, on one device.
    struct Oracle
    {
        //==============================================================================================================
        /// Loads the draw oracle library on `device`.
        explicit Oracle(Device created) : device(created), library(loadLibrary(created, "DrawOracle"))
        {
        }

        //==============================================================================================================
        /// The oracle's vertex and fragment pipeline of `primitive`, with `raster`.
        Pipeline pipeline(PrimitiveClass primitive = PrimitiveClass::Triangle, const RasterState& raster = {}) const
        {
            return pipelineOf(
                device,
                GraphicsPipelineDesc{
                    .vertex = { library, "oracleVertexMain" }, .fragment = { library, "oracleFragmentMain" }, .primitive = primitive, .raster = raster }
            );
        }

        Device device;         ///< The device.
        ShaderLibrary library; ///< The draw oracle library.
    };
} // namespace

//======================================================================================================================
TEST_CASE("draw writes each instance of each vertex range with drawId 0, firstVertex and firstInstance invisible", "[conformance]")
{
    test::requireGpu();
    // vertexIndex excludes firstVertex, so every range draws vertices 0 to 2 and the oracle's flat value is 0.
    const uint32_t firstVertex = GENERATE(0u, 3u, 999u);
    const uint32_t instances = GENERATE(1u, 4u);
    const uint32_t firstInstance = GENERATE(0u, 7u);
    CAPTURE(firstVertex, instances, firstInstance);
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    const Pipeline pipeline = oracle.pipeline();
    const Texture target = targetOf(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    list.beginRenderPass(passOf(colors));
    list.setPipeline(pipeline);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    list.draw(3, instances, firstVertex, firstInstance);
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    Image expected;
    for (uint32_t instance = 0; instance < instances; ++instance)
    {
        expected.set(0, instance * kInstanceRows, oracleValue(instance, 0));
    }
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("drawIndexed applies firstIndex with UInt16 and UInt32 indices, baseVertex invisible", "[conformance]")
{
    test::requireGpu();
    const IndexType type = GENERATE(IndexType::UInt16, IndexType::UInt32);
    // vertexIndex excludes baseVertex, so each draws the index values 3 to 5 and the oracle's flat value is 3.
    const int32_t baseVertex = GENERATE(0, 27, -3);
    CAPTURE(static_cast<uint32_t>(type), baseVertex);
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    const Pipeline pipeline = oracle.pipeline();
    const Texture target = targetOf(kit.device);
    // The first three indices are skipped by firstIndex; drawn, they would make a triangle of another first vertex.
    const std::array<uint32_t, 6> indices{ 9, 10, 11, 3, 4, 5 };
    const Buffer buffer = indexBuffer(kit, type, indices);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    list.beginRenderPass(passOf(colors));
    list.setPipeline(pipeline);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    list.drawIndexed(buffer, type, 3, 2, 3, baseVertex, 5);
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    Image expected;
    expected.set(0, 0, oracleValue(0, 3));
    expected.set(0, kInstanceRows, oracleValue(1, 3));
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("triangle strips and line lists draw with the topology set after setPipeline", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    const Pipeline triangles = oracle.pipeline();
    const Pipeline lines = oracle.pipeline(PrimitiveClass::Line);
    const Texture target = targetOf(kit.device);
    // As a list, 1 0 1 is a degenerate triangle; as a strip, 1 0 1 2 adds the triangle 0 1 2. As a list, 0 0 1 1 is two
    // degenerate lines; as a strip, it adds the line 0 1.
    const std::array<uint32_t, 10> indices{ 1, 0, 1, 2, 0, 1, 0, 0, 1, 1 };
    const Buffer buffer = indexBuffer(kit, IndexType::UInt16, indices);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    list.beginRenderPass(passOf(colors));
    list.setScissor(kFullRect);
    list.setPipeline(triangles);
    list.setPrimitiveTopology(PrimitiveTopology::TriangleStrip);
    list.setViewport(shifted(0, 0));
    list.drawIndexed(buffer, IndexType::UInt16, 4);
    // A line pipeline draws a line list.
    list.setPipeline(lines);
    list.setViewport(shifted(16, 0));
    list.drawIndexed(buffer, IndexType::UInt16, 2, 1, 4);
    list.setPrimitiveTopology(PrimitiveTopology::LineStrip);
    list.setViewport(shifted(32, 0));
    list.drawIndexed(buffer, IndexType::UInt16, 4, 1, 6);
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    Image expected;
    expected.set(0, 0, oracleValue(0, 0));
    expected.set(16, 0, oracleValue(0, 0));
    expected.set(32, 0, oracleValue(0, 0));
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("setPipeline resets the topology to its class's list form", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    const Pipeline triangles = oracle.pipeline();
    const Texture target = targetOf(kit.device);
    // As a list, 1 0 1 2 draws only a degenerate triangle; as a strip it draws the triangle 0 1 2.
    const std::array<uint32_t, 4> indices{ 1, 0, 1, 2 };
    const Buffer buffer = indexBuffer(kit, IndexType::UInt16, indices);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    list.beginRenderPass(passOf(colors));
    list.setScissor(kFullRect);
    list.setPipeline(triangles);
    list.setPrimitiveTopology(PrimitiveTopology::TriangleStrip);
    list.setPipeline(triangles);
    list.setViewport(shifted(0, 0));
    list.drawIndexed(buffer, IndexType::UInt16, 4);
    list.setPrimitiveTopology(PrimitiveTopology::TriangleStrip);
    list.setViewport(shifted(16, 0));
    list.drawIndexed(buffer, IndexType::UInt16, 4);
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    Image expected;
    expected.set(16, 0, oracleValue(0, 0));
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a viewport and a scissor limit what a full-target draw writes", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    const Pipeline solid = pipelineOf(
        kit.device, GraphicsPipelineDesc{ .vertex = { oracle.library, "fullscreenVertexMain" }, .fragment = { oracle.library, "solidFragmentMain" } }
    );
    const Texture target = targetOf(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    list.beginRenderPass(passOf(colors));
    list.setPipeline(solid);
    list.setViewport(Viewport{ .x = 32.0f, .y = 16.0f, .width = 128.0f, .height = 64.0f });
    list.setScissor(Rect2D{ 48, 24, 200, 200 });
    list.draw(3);
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    // The viewport covers [32, 160) by [16, 80) and the scissor [48, 248) by [24, 224).
    Image expected;
    for (uint32_t y = 24; y < 80; ++y)
    {
        for (uint32_t x = 48; x < 160; ++x)
        {
            expected.set(x, y, kSolid);
        }
    }
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a pass with a DontCare load stores what a draw over every pixel writes", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    const Pipeline solid = pipelineOf(
        kit.device, GraphicsPipelineDesc{ .vertex = { oracle.library, "fullscreenVertexMain" }, .fragment = { oracle.library, "solidFragmentMain" } }
    );
    const Texture target = targetOf(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    // Chapter 07: DontCare leaves the attachment undefined at the start of the pass, which the draw then overwrites.
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::DontCare, .store = StoreOp::Store } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    list.beginRenderPass(passOf(colors));
    list.setPipeline(solid);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    list.draw(3);
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    Image expected;
    for (uint32_t y = 0; y < kEdge; ++y)
    {
        for (uint32_t x = 0; x < kEdge; ++x)
        {
            expected.set(x, y, kSolid);
        }
    }
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("the stencil reference and a Replace operation mark exactly the drawn pixels", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    const StencilFaceState replace{ .pass = StencilOp::Replace, .compare = CompareOp::Always };
    const StencilFaceState equal{ .compare = CompareOp::Equal };
    // The marking draw writes the stencil alone; the test draw writes the solid color where the stencil equals the reference.
    const Pipeline mark = pipelineOf(
        kit.device,
        GraphicsPipelineDesc{ .vertex = { oracle.library, "oracleVertexMain" },
                              .fragment = { oracle.library, "oracleFragmentMain" },
                              .depthStencil = { .stencilTest = true, .front = replace, .back = replace },
                              .depthFormat = Format::D32FloatS8 },
        ColorTarget{ .format = Format::RGBA16Float, .writeMask = ColorWriteMask::None }
    );
    const Pipeline test = pipelineOf(
        kit.device,
        GraphicsPipelineDesc{ .vertex = { oracle.library, "fullscreenVertexMain" },
                              .fragment = { oracle.library, "solidFragmentMain" },
                              .depthStencil = { .stencilTest = true, .front = equal, .back = equal },
                              .depthFormat = Format::D32FloatS8 }
    );
    const Texture target = targetOf(kit.device);
    const Texture depth = targetOf(kit.device, Format::D32FloatS8, TextureUsage::DepthStencil);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    test::transition(list, depth, Layout::Undefined, Layout::DepthStencilWrite);
    list.beginRenderPass(passOf(
        colors,
        DepthAttachment{ .texture = depth, .load = LoadOp::Clear, .store = StoreOp::DontCare, .stencilLoad = LoadOp::Clear, .stencilStore = StoreOp::DontCare }
    ));
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    list.setPipeline(mark);
    // Four instance pixels take 7, then the first two take 5.
    list.setStencilReference(7);
    list.draw(3, 4);
    list.setStencilReference(5);
    list.draw(3, 2);
    list.setPipeline(test);
    list.draw(3);
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    Image expected;
    expected.set(0, 0, kSolid);
    expected.set(0, kInstanceRows, kSolid);
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("blend constants scale the solid color exactly", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    const BlendState blend{
        .enable = true, .srcColor = BlendFactor::Constant, .dstColor = BlendFactor::Zero, .srcAlpha = BlendFactor::Constant, .dstAlpha = BlendFactor::Zero
    };
    const Pipeline blended = pipelineOf(
        kit.device,
        GraphicsPipelineDesc{ .vertex = { oracle.library, "fullscreenVertexMain" }, .fragment = { oracle.library, "solidFragmentMain" } },
        ColorTarget{ .format = Format::RGBA16Float, .blend = blend }
    );
    const Texture target = targetOf(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    list.beginRenderPass(passOf(colors));
    list.setPipeline(blended);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    list.setBlendConstants(0.5f, 0.25f, 2.0f, 0.5f);
    list.draw(3);
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    Image expected;
    for (uint32_t y = 0; y < kEdge; ++y)
    {
        for (uint32_t x = 0; x < kEdge; ++x)
        {
            expected.set(x, y, { 0.125f, 0.125f, 1.5f, 0.5f });
        }
    }
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("depth bias moves a D32Float depth by the constant term", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    const DepthStencilState write{ .depthTest = true, .depthWrite = true, .depthCompare = CompareOp::Always };
    GraphicsPipelineDesc desc{ .vertex = { oracle.library, "fullscreenVertexMain" }, .depthStencil = write, .depthFormat = Format::D32Float };
    desc.raster.depthBias = true;
    desc.label = "test.biased";
    const Pipeline biased = *kit.device.createPipeline(desc);
    desc.raster.depthBias = false;
    desc.label = "test.plain";
    const Pipeline plain = *kit.device.createPipeline(desc);
    const Texture depth = targetOf(kit.device, Format::D32Float, TextureUsage::DepthStencil);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    test::transition(list, depth, Layout::Undefined, Layout::DepthStencilWrite);
    list.beginRenderPass(passOf({}, DepthAttachment{ .texture = depth, .load = LoadOp::Clear }));
    list.setViewport(kFull);
    // The left half with the bias, the right half without: a pipeline without depthBias ignores the values.
    list.setPipeline(biased);
    list.setDepthBias(4.0f, 0.0f, 0.0f);
    list.setScissor(Rect2D{ 0, 0, kEdge / 2, kEdge });
    list.draw(3);
    list.setPipeline(plain);
    list.setScissor(Rect2D{ kEdge / 2, 0, kEdge / 2, kEdge });
    list.draw(3);
    list.endRenderPass();
    test::transition(list, depth, Layout::DepthStencilWrite, Layout::CopySrc);
    kit.run(std::array{ list });
    const std::vector<std::byte> bytes = kit.readTexture(depth, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, Format::D32Float);
    REQUIRE(bytes.size() == size_t{ kEdge } * kEdge * sizeof(float));
    std::vector<float> depths(size_t{ kEdge } * kEdge);
    std::memcpy(depths.data(), bytes.data(), bytes.size());
    // Chapter 06: the constant is in units of the minimum resolvable difference, 2^-24 for a 32-bit float depth in [0.5, 1).
    const float expectedBiased = 0.5f + 4.0f * 0x1p-24f;
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < kEdge; ++y)
    {
        for (uint32_t x = 0; x < kEdge; ++x)
        {
            const float value = depths[size_t{ y } * kEdge + x];
            const float expected = x < kEdge / 2 ? expectedBiased : 0.5f;
            if (value != expected)
            {
                if (wrong == 0)
                {
                    UNSCOPED_INFO(std::format("first difference at ({}, {}): {:a}, expected {:a}", x, y, value, expected));
                }
                ++wrong;
            }
        }
    }
    CHECK(wrong == 0);
}

//======================================================================================================================
TEST_CASE("a Greater depth test over a depth cleared to 0 passes only nearer fragments, and GreaterOrEqual passes an equal one", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    // The first probe replaces what it passes over with the constant times the solid color; the second adds to it.
    const BlendState replace{
        .enable = true, .srcColor = BlendFactor::Constant, .dstColor = BlendFactor::Zero, .srcAlpha = BlendFactor::Constant, .dstAlpha = BlendFactor::Zero
    };
    const BlendState accumulate{
        .enable = true, .srcColor = BlendFactor::Constant, .dstColor = BlendFactor::One, .srcAlpha = BlendFactor::Constant, .dstAlpha = BlendFactor::One
    };
    const auto pipeline = [&](CompareOp compare, bool write, const BlendState& blend) {
        return pipelineOf(
            kit.device,
            GraphicsPipelineDesc{ .vertex = { oracle.library, "fullscreenVertexMain" },
                                  .fragment = { oracle.library, "solidFragmentMain" },
                                  .depthStencil = { .depthTest = true, .depthWrite = write, .depthCompare = compare },
                                  .depthFormat = Format::D32Float },
            ColorTarget{ .format = Format::RGBA16Float, .blend = blend }
        );
    };
    const Pipeline nearer = pipeline(CompareOp::Greater, true, {});
    const Pipeline probeGreater = pipeline(CompareOp::Greater, false, replace);
    const Pipeline probeEqualPasses = pipeline(CompareOp::GreaterOrEqual, false, accumulate);
    const Texture target = targetOf(kit.device);
    const Texture depth = targetOf(kit.device, Format::D32Float, TextureUsage::DepthStencil);
    // Clip z is 0.5, so the viewport's depth range puts it at (minDepth + maxDepth) / 2.
    const auto viewport = [](float minDepth, float maxDepth) {
        return Viewport{ .width = kEdge, .height = kEdge, .minDepth = minDepth, .maxDepth = maxDepth };
    };
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    test::transition(list, depth, Layout::Undefined, Layout::DepthStencilWrite);
    list.beginRenderPass(passOf(colors, DepthAttachment{ .texture = depth, .load = LoadOp::Clear, .store = StoreOp::Store, .clearDepth = 0.0f }));
    // Depth 0.25 passes Greater over a depth cleared to 0 everywhere; 0.75 then passes on the left half only.
    list.setPipeline(nearer);
    list.setViewport(viewport(0.0f, 0.5f));
    list.setScissor(kFullRect);
    list.draw(3);
    list.setViewport(viewport(0.5f, 1.0f));
    list.setScissor(Rect2D{ 0, 0, kEdge / 2, kEdge });
    list.draw(3);
    // 0.25 against 0.75 on the left and against an equal 0.25 on the right: Greater fails both. Were it to pass the right half, it would
    // replace the solid color with zero.
    list.setPipeline(probeGreater);
    list.setBlendConstants(0.0f, 0.0f, 0.0f, 0.0f);
    list.setViewport(viewport(0.0f, 0.5f));
    list.setScissor(kFullRect);
    list.draw(3);
    // GreaterOrEqual passes the equal depth on the right half only and adds half the solid color to what draw 3 left.
    list.setPipeline(probeEqualPasses);
    list.setBlendConstants(0.5f, 0.5f, 0.5f, 0.5f);
    list.setViewport(viewport(0.0f, 0.5f));
    list.setScissor(kFullRect);
    list.draw(3);
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    test::transition(list, depth, Layout::DepthStencilWrite, Layout::CopySrc);
    kit.run(std::array{ list });
    Image expected;
    for (uint32_t y = 0; y < kEdge; ++y)
    {
        for (uint32_t x = 0; x < kEdge; ++x)
        {
            expected.set(x, y, x < kEdge / 2 ? kSolid : std::array<float, 4>{ 0.375f, 0.75f, 1.125f, 1.5f });
        }
    }
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
    const std::vector<std::byte> bytes = kit.readTexture(depth, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, Format::D32Float);
    REQUIRE(bytes.size() == size_t{ kEdge } * kEdge * sizeof(float));
    std::vector<float> depths(size_t{ kEdge } * kEdge);
    std::memcpy(depths.data(), bytes.data(), bytes.size());
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < kEdge; ++y)
    {
        for (uint32_t x = 0; x < kEdge; ++x)
        {
            const float value = depths[size_t{ y } * kEdge + x];
            const float expectedDepth = x < kEdge / 2 ? 0.75f : 0.25f;
            if (value != expectedDepth)
            {
                if (wrong == 0)
                {
                    UNSCOPED_INFO(std::format("first difference at ({}, {}): {:a}, expected {:a}", x, y, value, expectedDepth));
                }
                ++wrong;
            }
        }
    }
    CHECK(wrong == 0);
}

//======================================================================================================================
TEST_CASE("back-face culling drops the triangles of one winding", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    // 0 1 2 winds clockwise in clip space (y up), and 3 5 4 counter-clockwise; their first vertices are 0 and 3.
    const std::array<uint32_t, 6> indices{ 0, 1, 2, 3, 5, 4 };
    const Buffer buffer = indexBuffer(kit, IndexType::UInt16, indices);
    struct Culling
    {
        RasterState raster;  ///< The pipeline's raster state.
        bool clockwiseDrawn; ///< The clockwise triangle is drawn.
        bool counterDrawn;   ///< The counter-clockwise triangle is drawn.
    };
    const std::array<Culling, 4> cases{
        Culling{ RasterState{ .cull = CullMode::None }, true, true },
        Culling{ RasterState{ .cull = CullMode::Back, .frontFace = FrontFace::CounterClockwise }, false, true },
        Culling{ RasterState{ .cull = CullMode::Back, .frontFace = FrontFace::Clockwise }, true, false },
        Culling{ RasterState{ .cull = CullMode::Front, .frontFace = FrontFace::CounterClockwise }, true, false },
    };
    std::array<Pipeline, 4> pipelines{};
    for (size_t i = 0; i < cases.size(); ++i)
    {
        pipelines[i] = oracle.pipeline(PrimitiveClass::Triangle, cases[i].raster);
    }
    const Texture target = targetOf(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    list.beginRenderPass(passOf(colors));
    list.setScissor(kFullRect);
    Image expected;
    for (uint32_t i = 0; i < cases.size(); ++i)
    {
        // Each case at its own column: the clockwise triangle in row 0, the counter-clockwise one in row 16.
        list.setPipeline(pipelines[i]);
        list.setViewport(shifted(16 * i, 0));
        list.drawIndexed(buffer, IndexType::UInt16, 3);
        list.setViewport(shifted(16 * i, 16));
        list.drawIndexed(buffer, IndexType::UInt16, 3, 1, 3);
        if (cases[i].clockwiseDrawn)
        {
            expected.set(16 * i, 0, oracleValue(0, 0));
        }
        if (cases[i].counterDrawn)
        {
            expected.set(16 * i, 16, oracleValue(0, 3));
        }
    }
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a pipeline that culls nothing applies its own winding to two-sided stencil after a culling pipeline", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    // 0 1 2 winds clockwise in clip space (y up), and 3 5 4 counter-clockwise.
    const std::array<uint32_t, 6> indices{ 0, 1, 2, 3, 5, 4 };
    const Buffer buffer = indexBuffer(kit, IndexType::UInt16, indices);
    const ColorTarget color{ .format = Format::RGBA16Float };
    // The culling pipeline's front faces wind clockwise; it tests no stencil.
    const Pipeline culling = pipelineOf(
        kit.device,
        GraphicsPipelineDesc{ .vertex = { oracle.library, "oracleVertexMain" },
                              .fragment = { oracle.library, "oracleFragmentMain" },
                              .raster = { .cull = CullMode::Back, .frontFace = FrontFace::Clockwise },
                              .depthFormat = Format::D32FloatS8 },
        color
    );
    // The marking pipeline culls nothing, its front faces wind counter-clockwise, and only a front face replaces the stencil.
    const StencilFaceState replace{ .pass = StencilOp::Replace };
    const StencilFaceState keep{};
    const Pipeline mark = pipelineOf(
        kit.device,
        GraphicsPipelineDesc{ .vertex = { oracle.library, "oracleVertexMain" },
                              .fragment = { oracle.library, "oracleFragmentMain" },
                              .raster = { .cull = CullMode::None, .frontFace = FrontFace::CounterClockwise },
                              .depthStencil = { .stencilTest = true, .front = replace, .back = keep },
                              .depthFormat = Format::D32FloatS8 },
        ColorTarget{ .format = Format::RGBA16Float, .writeMask = ColorWriteMask::None }
    );
    const StencilFaceState equal{ .compare = CompareOp::Equal };
    const Pipeline test = pipelineOf(
        kit.device,
        GraphicsPipelineDesc{ .vertex = { oracle.library, "fullscreenVertexMain" },
                              .fragment = { oracle.library, "solidFragmentMain" },
                              .depthStencil = { .stencilTest = true, .front = equal, .back = equal },
                              .depthFormat = Format::D32FloatS8 },
        color
    );
    const Texture target = targetOf(kit.device);
    const Texture depth = targetOf(kit.device, Format::D32FloatS8, TextureUsage::DepthStencil);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    test::transition(list, depth, Layout::Undefined, Layout::DepthStencilWrite);
    list.beginRenderPass(passOf(
        colors,
        DepthAttachment{ .texture = depth, .load = LoadOp::Clear, .store = StoreOp::DontCare, .stencilLoad = LoadOp::Clear, .stencilStore = StoreOp::DontCare }
    ));
    list.setScissor(kFullRect);
    list.setStencilReference(1);
    // The culling pipeline draws its front face, the clockwise triangle, at (48, 0).
    list.setPipeline(culling);
    list.setViewport(shifted(48, 0));
    list.drawIndexed(buffer, IndexType::UInt16, 3);
    // The marking pipeline draws the clockwise triangle at (0, 0) and the counter-clockwise one at (16, 0): only the second
    // is its front face, so only (16, 0) takes the reference.
    list.setPipeline(mark);
    list.setViewport(shifted(0, 0));
    list.drawIndexed(buffer, IndexType::UInt16, 3);
    list.setViewport(shifted(16, 0));
    list.drawIndexed(buffer, IndexType::UInt16, 3, 1, 3);
    list.setPipeline(test);
    list.setViewport(kFull);
    list.draw(3);
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    Image expected;
    expected.set(48, 0, oracleValue(0, 0));
    expected.set(16, 0, kSolid);
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("state set before the pass's first draw applies to that draw", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit.device);
    const ShaderLibrary fragment = loadLibrary(kit.device, "AbiFragment");
    // The oracle's vertex with AbiFragment, which samples the view and sampler its root names.
    const Pipeline sampled =
        pipelineOf(kit.device, GraphicsPipelineDesc{ .vertex = { oracle.library, "oracleVertexMain" }, .fragment = { fragment, "fragmentMain" } });
    const std::array<std::array<float, 4>, 3> texels{ std::array{ 0.25f, 0.5f, 0.75f, 1.0f },
                                                      std::array{ 2.0f, 4.0f, 8.0f, 16.0f },
                                                      std::array{ 0.125f, 1.5f, 3.0f, 0.5f } };
    const SamplerIndex sampler =
        *kit.device.createSampler(SamplerDesc{ .filter = SamplerFilter::Nearest, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge });
    struct FragmentRoot
    {
        uint32_t view;    ///< The sampled view.
        uint32_t sampler; ///< The sampler.
        uint32_t pad0;    ///< Padding.
        uint32_t pad1;    ///< Padding.
    };
    std::array<FragmentRoot, 3> roots{};
    for (size_t i = 0; i < texels.size(); ++i)
    {
        std::array<uint16_t, 4> halves{};
        for (size_t channel = 0; channel < 4; ++channel)
        {
            halves[channel] = halfOf(texels[i][channel]);
        }
        const std::array data{ TextureData{ std::as_bytes(std::span(halves)), sizeof(halves), 0 } };
        const Texture texture = *kit.device.createTexture(
            TextureDesc{ .format = Format::RGBA16Float, .extent = { 1, 1, 1 }, .usage = TextureUsage::Sampled, .label = "test.texel" }, data
        );
        roots[i] = FragmentRoot{ static_cast<uint32_t>(*kit.device.createView(texture, {})), static_cast<uint32_t>(sampler), 0, 0 };
    }
    const std::array targets{ targetOf(kit.device), targetOf(kit.device) };
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    std::array<Addr, 3> addresses{};
    for (size_t i = 0; i < roots.size(); ++i)
    {
        addresses[i] = list.uploadFrameData(std::as_bytes(std::span(&roots[i], 1)), 256);
    }
    // The root bound before the pass, then the pipeline, viewport and scissor recorded before its first draw.
    list.setRoot(addresses[0]);
    test::transition(list, targets[0], Layout::Undefined, Layout::ColorAttachment);
    test::transition(list, targets[1], Layout::Undefined, Layout::ColorAttachment);
    const std::array first{ ColorAttachment{ .texture = targets[0], .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(first));
    list.setPipeline(sampled);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    list.draw(3);
    list.endRenderPass();
    // A root bound inside the pass before its first draw, then another, with another viewport, between draws.
    const std::array second{ ColorAttachment{ .texture = targets[1], .load = LoadOp::Clear } };
    list.beginRenderPass(passOf(second));
    list.setRoot(addresses[1]);
    list.setPipeline(sampled);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    list.draw(3);
    list.setRoot(addresses[2]);
    list.setViewport(shifted(16, 0));
    list.draw(3);
    list.endRenderPass();
    test::transition(list, targets[0], Layout::ColorAttachment, Layout::CopySrc);
    test::transition(list, targets[1], Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    Image expectedFirst;
    expectedFirst.set(0, 0, texels[0]);
    Image expectedSecond;
    expectedSecond.set(0, 0, texels[1]);
    expectedSecond.set(16, 0, texels[2]);
    const std::string firstDifference = differences(readImage(kit, targets[0]), expectedFirst);
    INFO(firstDifference);
    CHECK(firstDifference.empty());
    const std::string secondDifference = differences(readImage(kit, targets[1]), expectedSecond);
    INFO(secondDifference);
    CHECK(secondDifference.empty());
}

//======================================================================================================================
TEST_CASE("drawMeshTasks draws each task group with drawId 0", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const ShaderLibrary library = loadLibrary(kit.device, "MeshOracle");
    const Pipeline mesh = pipelineOf(
        kit.device,
        GraphicsPipelineDesc{ .task = { library, "oracleTaskMain" }, .mesh = { library, "oracleMeshMain" }, .fragment = { library, "meshFragmentMain" } }
    );
    const Texture target = targetOf(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.draw");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    list.beginRenderPass(passOf(colors));
    list.setPipeline(mesh);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    list.drawMeshTasks(4, 1, 1);
    list.endRenderPass();
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    Image expected;
    for (uint32_t group = 0; group < 4; ++group)
    {
        expected.set(0, group * kInstanceRows, { 1.0f, static_cast<float>(group + 1), 0.0f, 1.0f });
    }
    const std::string difference = differences(readImage(kit, target), expected);
    INFO(difference);
    CHECK(difference.empty());
}
