//----------------------------------------------------------------------------------------------------------------------
/// @file GraphicsBarrierConformanceTests.cpp
/// @brief Observes the graphics barriers of chapter 09 on a GPU: producers and consumers across passes, dispatches, copies, counted draws, lists and queues.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
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
    // On the Apple GPU tried, with the graphics barriers dropped, the cases whose consumer is a dispatch or a counted draw's
    // kernel failed, and so did the copy after a pass once the compute encoder's opening barrier alone was dropped; a pass
    // consumer, after a pass, a copy or a dispatch, read the producer's values even without a barrier. Those cases check the
    // barrier's lowering, not its necessity.

    /// The `uint4` elements the copy kernel moves for a buffer producer: 64 MiB through 2^22 threads. A pass that pulled the
    /// tail without a barrier never overtook the kernel at 16 MiB, and overtook it once in seven runs at 64 MiB, which later
    /// runs did not reproduce: this case is one of those that check the lowering.
    constexpr uint32_t kCopyElements = uint32_t{ 1 } << 22;

    /// The bytes of a buffer producer's destination.
    constexpr uint64_t kCopyBytes = uint64_t{ kCopyElements } * 16;

    /// The threads of one copy kernel group, as `PullOracle.slang` declares them.
    constexpr uint32_t kCopyGroup = 256;

    /// The bytes at the end of a buffer producer's destination that the consumers read: records, a count or positions.
    constexpr uint64_t kTail = 4096;

    /// Where the tail starts in a buffer producer's destination.
    constexpr uint64_t kTailOffset = kCopyBytes - kTail;

    /// Where a count lies in a buffer producer's destination: its last 16 bytes.
    constexpr uint64_t kCountOffset = kCopyBytes - 16;

    /// The edge of a render producer's target, in texels.
    constexpr uint32_t kBigEdge = 2048;

    /// The fullscreen triangles a render producer draws over its target: 16 times 2^22 fragments. A depth producer of this
    /// size is overtaken by a dispatch without its barrier; 128 triangles made no pass consumer overtake a pass.
    constexpr uint32_t kLayers = 16;

    /// The edge of the oracle targets the consumers draw into, in pixels: the draw and mesh oracles place their triangles
    /// on a grid of this edge.
    constexpr uint32_t kEdge = 256;

    /// The color the solid and pull fragment entries write, exact in every format the cases use but `RGBA8Unorm`.
    constexpr std::array<float, 4> kSolid{ 0.25f, 0.5f, 0.75f, 1.0f };

    /// The color a producer's target holds before the producer runs, so a consumer that overtakes it reads another value.
    constexpr ClearColor kStale{ 2.0f, 4.0f, 8.0f, 16.0f };

    /// The viewport and scissor of an oracle target.
    constexpr Viewport kOracleViewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) };
    constexpr Rect2D kOracleRect{ 0, 0, kEdge, kEdge };

    /// The scissor of a render producer's target.
    constexpr Rect2D kBigRect{ 0, 0, kBigEdge, kBigEdge };

    /// The copy kernel's root.
    struct CopyRoot
    {
        Addr source;      ///< Where the kernel reads.
        Addr destination; ///< Where it writes.
        uint32_t count;   ///< The `uint4` elements it copies.
        uint32_t pad;     ///< Padding.
    };

    /// The pull vertex entry's root.
    struct PullRoot
    {
        Addr vertices; ///< The clip-space positions, one `float4` per vertex index.
    };

    /// AbiFragment's root.
    struct FragmentRoot
    {
        uint32_t view;    ///< The sampled view.
        uint32_t sampler; ///< The sampler.
        uint32_t pad0;    ///< Padding.
        uint32_t pad1;    ///< Padding.
    };

    /// AbiTextures' root.
    struct FetchRoot
    {
        uint32_t view;       ///< The fetched view.
        uint32_t sampler;    ///< The sampler.
        uint32_t comparison; ///< The comparison sampler.
        uint32_t pad;        ///< Padding.
        Addr output;         ///< Where the entry stores its `float4`.
    };

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

    //==================================================================================================================
    /// The bytes of `value` as one `RGBA16Float` texel.
    std::array<std::byte, 8> texelOf(const std::array<float, 4>& value)
    {
        std::array<uint16_t, 4> halves{};
        for (size_t channel = 0; channel < 4; ++channel)
        {
            halves[channel] = halfOf(value[channel]);
        }
        return std::bit_cast<std::array<std::byte, 8>>(halves);
    }

    //==================================================================================================================
    /// The bytes of `count` `RGBA16Float` texels of `value`.
    std::vector<std::byte> texelsOf(uint64_t count, const std::array<float, 4>& value)
    {
        const std::array<std::byte, 8> texel = texelOf(value);
        std::vector<std::byte> bytes;
        bytes.reserve(count * texel.size());
        for (uint64_t i = 0; i < count; ++i)
        {
            bytes.insert(bytes.end(), texel.begin(), texel.end());
        }
        return bytes;
    }

    //==================================================================================================================
    /// The bytes of `values`.
    template<class T, size_t N> std::vector<std::byte> bytesOf(std::span<T, N> values)
    {
        const std::span<const std::byte> bytes = std::as_bytes(values);
        return { bytes.begin(), bytes.end() };
    }

    //==================================================================================================================
    /// Describes where `actual` differs from `expected`, both rows of `width` texels of `texelBytes` bytes: the first eight
    /// texels and how many differ; empty when they are equal.
    std::string differences(std::span<const std::byte> actual, std::span<const std::byte> expected, uint32_t texelBytes, uint32_t width)
    {
        if (actual.size() != expected.size())
        {
            return std::format("{} bytes read back, expected {}", actual.size(), expected.size());
        }
        std::string text;
        uint64_t found = 0;
        for (size_t texel = 0; texel < expected.size() / texelBytes; ++texel)
        {
            const size_t at = texel * texelBytes;
            if (std::memcmp(&actual[at], &expected[at], texelBytes) == 0)
            {
                continue;
            }
            if (++found <= 8)
            {
                uint64_t got = 0;
                uint64_t wanted = 0;
                std::memcpy(&got, &actual[at], texelBytes);
                std::memcpy(&wanted, &expected[at], texelBytes);
                text += std::format("({}, {}): {:016x}, expected {:016x}\n", texel % width, texel / width, got, wanted);
            }
        }
        if (found > 0)
        {
            text += std::format("{} texels differ", found);
        }
        return text;
    }

    /// An `RGBA16Float` oracle target's bytes, zero but for the texels `set` names.
    struct OracleImage
    {
        //==============================================================================================================
        /// Sets the texel at cell `cell`, `x` plus `kEdge` times `y`, to `value`.
        void set(uint32_t cell, const std::array<float, 4>& value)
        {
            const std::array<std::byte, 8> texel = texelOf(value);
            std::memcpy(bytes.data() + size_t{ cell } * texel.size(), texel.data(), texel.size());
        }

        std::vector<std::byte> bytes = std::vector<std::byte>(size_t{ kEdge } * kEdge * 8); ///< In texel order.
    };

    //==================================================================================================================
    /// Loads the conformance library `name` on `device`.
    ShaderLibrary loadLibrary(Device device, std::string_view name)
    {
        const std::vector<std::byte> blob = test::shaderLibraryFile(name);
        return *device.loadShaderLibrary(blob, name);
    }

    //==================================================================================================================
    /// Creates the graphics pipeline `desc` describes over the color targets `targets`; fails the case when it is refused.
    Pipeline graphicsOf(Device device, GraphicsPipelineDesc desc, std::span<const ColorTarget> targets)
    {
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
    /// Creates the compute pipeline of `entry` of `library`.
    Pipeline computeOf(Device device, ShaderLibrary library, std::string_view entry)
    {
        return *device.createPipeline(ComputePipelineDesc{ .compute = { library, entry }, .label = "test.compute" });
    }

    //==================================================================================================================
    /// Creates a 2D texture of `edge` texels square.
    Texture textureOf(Device device, uint32_t edge, Format format, TextureUsage usage, uint32_t samples = 1)
    {
        return *device.createTexture(TextureDesc{ .format = format, .extent = { edge, edge, 1 }, .samples = samples, .usage = usage, .label = "test.texture" });
    }

    //==================================================================================================================
    /// Runs a list that moves the new texture `texture` from `Undefined` to `layout`, through `CopyDst` and a clear to
    /// `clear` when one is given, so the texture holds a value a consumer overtaking its producer would read.
    void prepare(test::GpuKit& kit, Texture texture, Layout layout, const ClearValue* clear = nullptr)
    {
        CommandList list = kit.list(QueueType::Graphics, "test.prepare");
        if (clear != nullptr)
        {
            test::transition(list, texture, Layout::Undefined, Layout::CopyDst);
            list.clearTexture(texture, {}, *clear);
            test::transition(list, texture, Layout::CopyDst, layout);
        }
        else
        {
            test::transition(list, texture, Layout::Undefined, layout);
        }
        kit.run(std::array{ list });
    }

    //==================================================================================================================
    /// A barrier on `texture` from `srcStage` and `srcAccess` in `oldLayout` to `dstStage` and `dstAccess` in `newLayout`.
    TextureBarrier textureBarrier(Texture texture, Stage srcStage, Access srcAccess, Layout oldLayout, Stage dstStage, Access dstAccess, Layout newLayout)
    {
        return TextureBarrier{ .texture = texture,
                               .srcStage = srcStage,
                               .srcAccess = srcAccess,
                               .dstStage = dstStage,
                               .dstAccess = dstAccess,
                               .oldLayout = oldLayout,
                               .newLayout = newLayout };
    }

    //==================================================================================================================
    /// A barrier on `buffer` from compute shader writes to `dstStage` and `dstAccess`.
    BufferBarrier computeWritten(Buffer buffer, Stage dstStage, Access dstAccess)
    {
        return BufferBarrier{
            .buffer = buffer, .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = dstStage, .dstAccess = dstAccess
        };
    }

    /// The oracles one case uses, on one kit: the libraries load once, the pipelines on demand.
    struct Oracles
    {
        //==============================================================================================================
        /// Loads the draw, pull and fragment oracle libraries on `kit`'s device.
        explicit Oracles(test::GpuKit& created)
            : kit(created), draw(loadLibrary(created.device, "DrawOracle")), pull(loadLibrary(created.device, "PullOracle")),
              fragment(loadLibrary(created.device, "AbiFragment")), copy(computeOf(created.device, pull, "copyMain"))
        {
        }

        //==============================================================================================================
        /// The fullscreen solid pipeline over one color target of `format` with `samples`.
        Pipeline solid(Format format, uint32_t samples = 1) const
        {
            const std::array targets{ ColorTarget{ .format = format } };
            return graphicsOf(
                kit.device,
                GraphicsPipelineDesc{ .vertex = { draw, "fullscreenVertexMain" }, .fragment = { draw, "solidFragmentMain" }, .samples = samples },
                targets
            );
        }

        //==============================================================================================================
        /// The draw oracle's pipeline over one color target of `format`.
        Pipeline oracle(Format format = Format::RGBA16Float) const
        {
            const std::array targets{ ColorTarget{ .format = format } };
            return graphicsOf(kit.device, GraphicsPipelineDesc{ .vertex = { draw, "oracleVertexMain" }, .fragment = { draw, "oracleFragmentMain" } }, targets);
        }

        //==============================================================================================================
        /// The pull oracle's pipeline over one `RGBA16Float` target.
        Pipeline pulled() const
        {
            const std::array targets{ ColorTarget{ .format = Format::RGBA16Float } };
            return graphicsOf(kit.device, GraphicsPipelineDesc{ .vertex = { pull, "pullVertexMain" }, .fragment = { pull, "pullFragmentMain" } }, targets);
        }

        //==============================================================================================================
        /// The draw oracle's vertex with AbiFragment, which samples the view its root names, over one `RGBA16Float` target.
        Pipeline sampled() const
        {
            const std::array targets{ ColorTarget{ .format = Format::RGBA16Float } };
            return graphicsOf(kit.device, GraphicsPipelineDesc{ .vertex = { draw, "oracleVertexMain" }, .fragment = { fragment, "fragmentMain" } }, targets);
        }

        //==============================================================================================================
        /// A nearest, clamped sampler.
        SamplerIndex nearest() const
        {
            return *kit.device.createSampler(
                SamplerDesc{
                    .filter = SamplerFilter::Nearest, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge, .label = "test.nearest" }
            );
        }

        //==============================================================================================================
        /// Records on `list` the copy kernel moving `count` `uint4` elements from `source` to `destination`.
        void copyOn(CommandList list, Addr source, Addr destination, uint32_t count) const
        {
            list.setPipeline(copy);
            list.pushRoot(CopyRoot{ .source = source, .destination = destination, .count = count, .pad = 0 });
            list.dispatch((count + kCopyGroup - 1) / kCopyGroup, 1, 1);
        }

        //==============================================================================================================
        /// Records on `list` the slow buffer producer: the copy kernel moving every byte of `source` into `destination`.
        void produceOn(CommandList list, Buffer source, Buffer destination) const
        {
            copyOn(list, kit.device.address(source), kit.device.address(destination), kCopyElements);
        }

        //==============================================================================================================
        /// Records on `list` the slow render producer: a pass over the `edge` square `target` that clears it to zero and draws
        /// `kLayers` fullscreen triangles of the solid color with `pipeline`, resolving into `resolve` when it is given.
        static void renderOn(CommandList list, Texture target, Pipeline pipeline, Texture resolve = {}, uint32_t edge = kBigEdge)
        {
            const std::array colors{ ColorAttachment{
                .texture = target, .load = LoadOp::Clear, .store = resolve.bits == 0 ? StoreOp::Store : StoreOp::DontCare, .resolve = resolve } };
            const Rect2D area{ 0, 0, edge, edge };
            list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = area, .label = "test.producer" });
            list.setPipeline(pipeline);
            list.setViewport(Viewport{ .width = static_cast<float>(edge), .height = static_cast<float>(edge) });
            list.setScissor(area);
            list.draw(3, kLayers);
            list.endRenderPass();
        }

        //==============================================================================================================
        /// Records on `list` a pass over the oracle target `target` that clears it, binds `pipeline` and pushes `root`, then
        /// draws three vertices: one oracle triangle at its first pixel, or the triangle a pull root names.
        template<class Root> static void drawOn(CommandList list, Texture target, Pipeline pipeline, const Root& root)
        {
            const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
            list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kOracleRect, .label = "test.consumer" });
            list.setPipeline(pipeline);
            list.setViewport(kOracleViewport);
            list.setScissor(kOracleRect);
            list.pushRoot(root);
            list.draw(3);
            list.endRenderPass();
        }

        //==============================================================================================================
        /// Records on `list` a pass over the oracle target `target` that samples `view` at its centre into the first pixel.
        void sampleOn(CommandList list, Texture target, Pipeline pipeline, Texture view) const
        {
            const ViewIndex index = *kit.device.createView(view, {});
            drawOn(list, target, pipeline, FragmentRoot{ static_cast<uint32_t>(index), static_cast<uint32_t>(nearest()), 0, 0 });
        }

        //==============================================================================================================
        /// Creates an `RGBA16Float` oracle target in `ColorAttachment`.
        Texture target() const
        {
            const Texture texture = textureOf(kit.device, kEdge, Format::RGBA16Float, TextureUsage::RenderTarget);
            prepare(kit, texture, Layout::ColorAttachment);
            return texture;
        }

        //==============================================================================================================
        /// Creates a `kBigEdge` square `RGBA16Float` render producer target with `usage` besides `RenderTarget`, holding
        /// `kStale`, in `ColorAttachment`.
        Texture bigTarget(TextureUsage usage = TextureUsage::None) const
        {
            const Texture texture = textureOf(kit.device, kBigEdge, Format::RGBA16Float, TextureUsage::RenderTarget | usage);
            const ClearValue stale{ .color = kStale };
            prepare(kit, texture, Layout::ColorAttachment, &stale);
            return texture;
        }

        //==============================================================================================================
        /// Reads `texture`, an `RGBA16Float` oracle target, back.
        std::vector<std::byte> read(Texture texture) const
        {
            return kit.readTexture(texture, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, Format::RGBA16Float);
        }

        test::GpuKit& kit;      ///< The kit.
        ShaderLibrary draw;     ///< The draw oracle library.
        ShaderLibrary pull;     ///< The pull oracle library.
        ShaderLibrary fragment; ///< AbiFragment.
        Pipeline copy;          ///< The copy kernel.
    };

    //==================================================================================================================
    /// The source of a buffer producer: `kCopyBytes` bytes in an Upload buffer, zero but for `tail` from `kTailOffset` and
    /// `count` at `kCountOffset`.
    Buffer sourceOf(test::GpuKit& kit, std::span<const std::byte> tail, uint32_t count = 0)
    {
        std::vector<std::byte> bytes(kCopyBytes, std::byte{ 0 });
        REQUIRE(tail.size() <= kTail - 16);
        std::memcpy(bytes.data() + kTailOffset, tail.data(), tail.size());
        std::memcpy(bytes.data() + kCountOffset, &count, sizeof(count));
        return kit.buffer(kCopyBytes, Memory::Upload, BufferUsage::None, bytes);
    }

    //==================================================================================================================
    /// A buffer producer's destination: `kCopyBytes` zero bytes in GPU memory that shaders write, with `usage` besides.
    Buffer destinationOf(test::GpuKit& kit, BufferUsage usage = BufferUsage::None)
    {
        const std::vector<std::byte> zeroes(kCopyBytes, std::byte{ 0 });
        return kit.buffer(kCopyBytes, Memory::Gpu, BufferUsage::ShaderWrite | usage, zeroes);
    }

    /// The clip-space corners of a triangle over the whole target.
    constexpr std::array<std::array<float, 4>, 3> kFullscreen{ std::array{ -1.0f, -1.0f, 0.0f, 1.0f },
                                                               std::array{ 3.0f, -1.0f, 0.0f, 1.0f },
                                                               std::array{ -1.0f, 3.0f, 0.0f, 1.0f } };

    //==================================================================================================================
    /// An oracle target's image when the solid color covers it.
    OracleImage solidImage()
    {
        OracleImage image;
        for (uint32_t cell = 0; cell < kEdge * kEdge; ++cell)
        {
            image.set(cell, kSolid);
        }
        return image;
    }

    //==================================================================================================================
    /// An oracle target's image when one oracle triangle wrote `value` at its first pixel.
    OracleImage firstPixel(const std::array<float, 4>& value)
    {
        OracleImage image;
        image.set(0, value);
        return image;
    }

    //==================================================================================================================
    /// Checks that `actual`, an oracle target read back, equals `expected`.
    void checkImage(const std::vector<std::byte>& actual, const OracleImage& expected)
    {
        const std::string difference = differences(actual, expected.bytes, 8, kEdge);
        INFO(difference);
        CHECK(difference.empty());
    }
} // namespace

//======================================================================================================================
TEST_CASE("a color attachment written in one pass is sampled by the next pass's fragment stage", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline solid = oracles.solid(Format::RGBA16Float);
    const Pipeline sampled = oracles.sampled();
    const Texture produced = oracles.bigTarget(TextureUsage::Sampled);
    const Texture target = oracles.target();
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    Oracles::renderOn(list, produced, solid);
    list.barrier(
        {},
        {},
        std::array{ textureBarrier(
            produced,
            Stage::ColorAttachment,
            Access::ColorAttachmentWrite,
            Layout::ColorAttachment,
            Stage::FragmentShader,
            Access::ShaderRead,
            Layout::ShaderRead
        ) }
    );
    oracles.sampleOn(list, target, sampled, produced);
    kit.run(std::array{ list });
    checkImage(oracles.read(target), firstPixel(kSolid));
}

//======================================================================================================================
TEST_CASE("a compute shader's buffer writes are pulled by the next pass's vertex stage", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline pulled = oracles.pulled();
    const Buffer source = sourceOf(kit, std::as_bytes(std::span(kFullscreen)));
    // The destination starts as zeroes, a degenerate triangle that draws nothing: only the kernel's writes make it draw.
    const Buffer positions = destinationOf(kit);
    const Texture target = oracles.target();
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    oracles.produceOn(list, source, positions);
    list.barrier({}, std::array{ computeWritten(positions, Stage::VertexShader, Access::ShaderRead) });
    Oracles::drawOn(list, target, pulled, PullRoot{ kit.device.address(positions, kTailOffset) });
    kit.run(std::array{ list });
    checkImage(oracles.read(target), solidImage());
}

//======================================================================================================================
TEST_CASE("a compute shader's record writes are read by a counted draw", "[conformance]")
{
    test::requireGpu();
    // One record runs the writer kernel and a native indirect draw; 64 run the encoding kernel and an execute.
    const uint32_t maxCount = GENERATE(1u, 64u);
    CAPTURE(maxCount);
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline pipeline = oracles.oracle();
    std::vector<DrawIndirectArgs> records;
    for (uint32_t i = 0; i < maxCount; ++i)
    {
        records.push_back(DrawIndirectArgs{ .drawId = 2 * i + 5, .vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0 });
    }
    const uint32_t count = maxCount == 1 ? 1 : 37;
    const Buffer source = sourceOf(kit, bytesOf(std::span<const DrawIndirectArgs>(records)), count);
    // The records and the count start as zeroes, which draw nothing.
    const Buffer args = destinationOf(kit, BufferUsage::IndirectArgs);
    const Texture target = oracles.target();
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    oracles.produceOn(list, source, args);
    // The counted draw's kernel reads the records and the count on a compute encoder ahead of the render encoder, which
    // emits the barrier's dispatch part; the render encoder emits the rest.
    list.barrier({}, std::array{ computeWritten(args, Stage::IndirectArgs, Access::IndirectArgs) });
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kOracleRect, .label = "test.consumer" });
    list.setPipeline(pipeline);
    list.setViewport(kOracleViewport);
    list.setScissor(kOracleRect);
    list.drawIndirect(kit.device.address(args, kTailOffset), maxCount, kit.device.address(args, kCountOffset));
    list.endRenderPass();
    kit.run(std::array{ list });
    OracleImage expected;
    for (uint32_t i = 0; i < std::min(count, maxCount); ++i)
    {
        expected.set(records[i].drawId, { static_cast<float>(records[i].drawId + 1), 1.0f, 1.0f, 1.0f });
    }
    checkImage(oracles.read(target), expected);
}

//======================================================================================================================
TEST_CASE("a compute shader's record writes are read by a lone mesh record", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    if (!kit.device.capabilities().meshTasksIndirect)
    {
        SKIP("the device reports no meshTasksIndirect");
    }
    const Oracles oracles(kit);
    const ShaderLibrary mesh = loadLibrary(kit.device, "MeshOracle");
    const std::array targets{ ColorTarget{ .format = Format::RGBA16Float } };
    const Pipeline pipeline = graphicsOf(
        kit.device,
        GraphicsPipelineDesc{ .task = { mesh, "oracleTaskMain" }, .mesh = { mesh, "oracleMeshMain" }, .fragment = { mesh, "meshFragmentMain" } },
        targets
    );
    // One record of drawId 9 and four task groups, read by the native indirect mesh draw: a null count and maxCount 1.
    const std::array record{ DrawMeshTasksIndirectArgs{ .drawId = 9, .x = 4, .y = 1, .z = 1 } };
    const Buffer source = sourceOf(kit, bytesOf(std::span<const DrawMeshTasksIndirectArgs>(record)));
    const Buffer args = destinationOf(kit, BufferUsage::IndirectArgs);
    const Texture target = oracles.target();
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    oracles.produceOn(list, source, args);
    list.barrier({}, std::array{ computeWritten(args, Stage::IndirectArgs, Access::IndirectArgs) });
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kOracleRect, .label = "test.consumer" });
    list.setPipeline(pipeline);
    list.setViewport(kOracleViewport);
    list.setScissor(kOracleRect);
    list.drawMeshTasksIndirect(kit.device.address(args, kTailOffset), 1);
    list.endRenderPass();
    kit.run(std::array{ list });
    // Each task group advances the oracle's pixel by 64 rows.
    OracleImage expected;
    for (uint32_t group = 0; group < 4; ++group)
    {
        expected.set(9 + group * 64 * kEdge, { 10.0f, static_cast<float>(group + 1), 0.0f, 1.0f });
    }
    checkImage(oracles.read(target), expected);
}

//======================================================================================================================
TEST_CASE("a depth attachment written in one pass is sampled by a compute dispatch", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    const DepthStencilState write{ .depthTest = true, .depthWrite = true, .depthCompare = CompareOp::Always };
    const Pipeline depthOnly = graphicsOf(
        kit.device, GraphicsPipelineDesc{ .vertex = { oracles.draw, "fullscreenVertexMain" }, .depthStencil = write, .depthFormat = Format::D32Float }, {}
    );
    const ShaderLibrary textures = loadLibrary(kit.device, "AbiTextures");
    const Pipeline compare = computeOf(kit.device, textures, "compareMain");
    const Texture depth = textureOf(kit.device, kBigEdge, Format::D32Float, TextureUsage::DepthStencil | TextureUsage::Sampled);
    // compareMain passes when its reference, 0.5, is at most the stored depth: the stale 0.25 and the pass's clear to 0 fail,
    // and only the draws' 0.75 passes.
    const ClearValue stale{ .depth = 0.25f };
    prepare(kit, depth, Layout::DepthStencilWrite, &stale);
    const ViewIndex view = *kit.device.createView(depth, TextureViewDesc{ .aspect = TextureAspect::Depth });
    const SamplerIndex comparison = *kit.device.createSampler(
        SamplerDesc{ .filter = SamplerFilter::Nearest,
                     .mipFilter = SamplerFilter::Nearest,
                     .address = SamplerAddress::ClampToEdge,
                     .compareEnable = true,
                     .compare = CompareOp::LessOrEqual,
                     .label = "test.comparison" }
    );
    const std::vector<std::byte> zeroes(16, std::byte{ 0 });
    const Buffer output = kit.buffer(16, Memory::Gpu, BufferUsage::ShaderWrite, zeroes);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    list.beginRenderPass(
        RenderPassDesc{ .depth = DepthAttachment{ .texture = depth, .load = LoadOp::Clear, .clearDepth = 0.0f }, .area = kBigRect, .label = "test.producer" }
    );
    list.setPipeline(depthOnly);
    // The viewport maps the triangle's clip-space depth of 0.5 to 0.75.
    list.setViewport(Viewport{ .width = static_cast<float>(kBigEdge), .height = static_cast<float>(kBigEdge), .minDepth = 0.5f, .maxDepth = 1.0f });
    list.setScissor(kBigRect);
    list.draw(3, kLayers);
    list.endRenderPass();
    list.barrier(
        {},
        {},
        std::array{ textureBarrier(
            depth, Stage::DepthStencil, Access::DepthStencilWrite, Layout::DepthStencilWrite, Stage::ComputeShader, Access::ShaderRead, Layout::DepthStencilRead
        ) }
    );
    list.setPipeline(compare);
    list.pushRoot(
        FetchRoot{
            .view = static_cast<uint32_t>(view), .sampler = 0, .comparison = static_cast<uint32_t>(comparison), .pad = 0, .output = kit.device.address(output) }
    );
    list.dispatch(1, 1, 1);
    kit.run(std::array{ list });
    const std::array<float, 4> passed{ 1.0f, 0.0f, 0.0f, 0.0f };
    CHECK(kit.readBuffer(output) == bytesOf(std::span<const float>(passed)));
}

//======================================================================================================================
TEST_CASE("a copy into a texture is loaded by the next pass's attachment", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline pipeline = oracles.oracle(Format::RGBA8Unorm);
    constexpr uint32_t kRow = kBigEdge * 4;
    const Texture texture = textureOf(kit.device, kBigEdge, Format::RGBA8Unorm, TextureUsage::RenderTarget);
    prepare(kit, texture, Layout::CopyDst);
    // 16 MiB of texels whose bytes are never 0 or 255, through a copy from an Upload buffer.
    std::vector<std::byte> texels = test::pattern(uint64_t{ kRow } * kBigEdge, 4);
    const Buffer upload = kit.buffer(texels.size(), Memory::Upload, BufferUsage::None, texels);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    list.copyBufferToTexture(upload, BufferTextureLayout{ .rowPitch = kRow }, texture, TextureRegion{ .extent = { kBigEdge, kBigEdge, 1 } });
    list.barrier(
        {},
        {},
        std::array{ textureBarrier(
            texture,
            Stage::Copy,
            Access::CopyWrite,
            Layout::CopyDst,
            Stage::ColorAttachment,
            Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
            Layout::ColorAttachment
        ) }
    );
    // The pass loads every texel and stores it back; one oracle triangle writes 1.0 in every channel of its first.
    const std::array colors{ ColorAttachment{ .texture = texture, .load = LoadOp::Load } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kBigRect, .label = "test.consumer" });
    list.setPipeline(pipeline);
    list.setViewport(kOracleViewport);
    list.setScissor(kBigRect);
    list.draw(3);
    list.endRenderPass();
    kit.run(std::array{ list });
    std::fill(texels.begin(), texels.begin() + 4, std::byte{ 0xFF });
    const std::vector<std::byte> actual = kit.readTexture(texture, TextureRegion{ .extent = { kBigEdge, kBigEdge, 1 } }, Format::RGBA8Unorm);
    const std::string difference = differences(actual, texels, 4, kBigEdge);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a pass's attachment is copied to a buffer after the barrier", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline solid = oracles.solid(Format::RGBA16Float);
    const Texture produced = oracles.bigTarget();
    constexpr uint32_t kRow = kBigEdge * 8;
    constexpr uint32_t kRows = 4;
    const Buffer readback = kit.buffer(uint64_t{ kRow } * kRows, Memory::Readback);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    Oracles::renderOn(list, produced, solid);
    list.barrier(
        {},
        {},
        std::array{ textureBarrier(
            produced, Stage::ColorAttachment, Access::ColorAttachmentWrite, Layout::ColorAttachment, Stage::Copy, Access::CopyRead, Layout::CopySrc
        ) }
    );
    list.copyTextureToBuffer(
        produced, TextureRegion{ .offset = { 0, kBigEdge - kRows, 0 }, .extent = { kBigEdge, kRows, 1 } }, readback, BufferTextureLayout{ .rowPitch = kRow }
    );
    kit.run(std::array{ list });
    const std::string difference = differences(kit.read(readback), texelsOf(uint64_t{ kBigEdge } * kRows, kSolid), 8, kBigEdge);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a resolve target is sampled after the barrier", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    // Four samples of a quarter of the producer's usual area: the multisample target holds as many samples as it has texels.
    constexpr uint32_t kResolveEdge = kBigEdge / 2;
    const Pipeline solid = oracles.solid(Format::RGBA16Float, 4);
    const Pipeline sampled = oracles.sampled();
    const Texture multisample = textureOf(kit.device, kResolveEdge, Format::RGBA16Float, TextureUsage::RenderTarget, 4);
    prepare(kit, multisample, Layout::ColorAttachment);
    const Texture resolved = textureOf(kit.device, kResolveEdge, Format::RGBA16Float, TextureUsage::RenderTarget | TextureUsage::Sampled);
    const ClearValue stale{ .color = kStale };
    prepare(kit, resolved, Layout::ColorAttachment, &stale);
    const Texture target = oracles.target();
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    Oracles::renderOn(list, multisample, solid, resolved, kResolveEdge);
    list.barrier(
        {},
        {},
        std::array{
            textureBarrier(resolved, Stage::Resolve, Access::Resolve, Layout::ColorAttachment, Stage::FragmentShader, Access::ShaderRead, Layout::ShaderRead) }
    );
    oracles.sampleOn(list, target, sampled, resolved);
    kit.run(std::array{ list });
    checkImage(oracles.read(target), firstPixel(kSolid));
}

//======================================================================================================================
TEST_CASE("a barrier at the start of a list orders the previous list's render pass on the queue", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline solid = oracles.solid(Format::RGBA16Float);
    const Texture produced = oracles.bigTarget();
    constexpr uint32_t kRow = kBigEdge * 8;
    constexpr uint32_t kRows = 4;
    const Buffer readback = kit.buffer(uint64_t{ kRow } * kRows, Memory::Readback);
    CommandList producer = kit.list(QueueType::Graphics, "test.producer");
    Oracles::renderOn(producer, produced, solid);
    CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
    consumer.barrier(
        {},
        {},
        std::array{ textureBarrier(
            produced, Stage::ColorAttachment, Access::ColorAttachmentWrite, Layout::ColorAttachment, Stage::Copy, Access::CopyRead, Layout::CopySrc
        ) }
    );
    consumer.copyTextureToBuffer(
        produced, TextureRegion{ .offset = { 0, kBigEdge - kRows, 0 }, .extent = { kBigEdge, kRows, 1 } }, readback, BufferTextureLayout{ .rowPitch = kRow }
    );
    // Both lists go in one submission, so only the barrier orders them.
    kit.run(std::array{ producer, consumer });
    const std::string difference = differences(kit.read(readback), texelsOf(uint64_t{ kBigEdge } * kRows, kSolid), 8, kBigEdge);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a barrier at the end of a list orders the next list's pass on the queue", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline solid = oracles.solid(Format::RGBA16Float);
    const Pipeline sampled = oracles.sampled();
    const Texture produced = oracles.bigTarget(TextureUsage::Sampled);
    const Texture target = oracles.target();
    CommandList producer = kit.list(QueueType::Graphics, "test.producer");
    Oracles::renderOn(producer, produced, solid);
    producer.barrier(
        {},
        {},
        std::array{ textureBarrier(
            produced,
            Stage::ColorAttachment,
            Access::ColorAttachmentWrite,
            Layout::ColorAttachment,
            Stage::FragmentShader,
            Access::ShaderRead,
            Layout::ShaderRead
        ) }
    );
    CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
    oracles.sampleOn(consumer, target, sampled, produced);
    kit.run(std::array{ producer, consumer });
    checkImage(oracles.read(target), firstPixel(kSolid));
}

//======================================================================================================================
TEST_CASE("a barrier whose destination names compute and render stages holds back both kinds of encoder after it", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline pulled = oracles.pulled();
    const std::vector<std::byte> tail = bytesOf(std::span(kFullscreen));
    const Buffer source = sourceOf(kit, tail);
    const Buffer produced = destinationOf(kit);
    // The dispatch consumer copies the tail, positions and zeroes, into a buffer that starts with other bytes.
    const Buffer copied = kit.buffer(kTail, Memory::Gpu, BufferUsage::ShaderWrite, test::pattern(kTail, 6));
    const Texture target = oracles.target();
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    oracles.produceOn(list, source, produced);
    list.barrier({}, std::array{ computeWritten(produced, Stage::ComputeShader | Stage::VertexShader, Access::ShaderRead) });
    const auto dispatchConsumer = [&] { oracles.copyOn(list, kit.device.address(produced, kTailOffset), kit.device.address(copied), kTail / 16); };
    const auto passConsumer = [&] { Oracles::drawOn(list, target, pulled, PullRoot{ kit.device.address(produced, kTailOffset) }); };
    SECTION("the dispatch first")
    {
        dispatchConsumer();
        passConsumer();
    }
    SECTION("the pass first")
    {
        passConsumer();
        dispatchConsumer();
    }
    kit.run(std::array{ list });
    std::vector<std::byte> expected(kTail, std::byte{ 0 });
    std::memcpy(expected.data(), tail.data(), tail.size());
    CHECK(kit.readBuffer(copied) == expected);
    checkImage(oracles.read(target), solidImage());
}

//======================================================================================================================
TEST_CASE("an attachment rendered on the graphics queue is read on the compute queue after a fence", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline solid = oracles.solid(Format::RGBA16Float);
    const ShaderLibrary textures = loadLibrary(kit.device, "AbiTextures");
    const Pipeline fetch = computeOf(kit.device, textures, "texture2DMain");
    const Texture produced = oracles.bigTarget(TextureUsage::Sampled);
    const ViewIndex view = *kit.device.createView(produced, {});
    const std::vector<std::byte> zeroes(16, std::byte{ 0 });
    const Buffer output = kit.buffer(16, Memory::Gpu, BufferUsage::ShaderWrite, zeroes);
    CommandList graphics = kit.list(QueueType::Graphics, "test.graphics");
    Oracles::renderOn(graphics, produced, solid);
    // The handoff leaves the texture in ShaderRead, which both queues accept; the fence, not the barrier, orders the
    // compute queue's read after the pass.
    graphics.barrier(
        {},
        {},
        std::array{ textureBarrier(
            produced, Stage::ColorAttachment, Access::ColorAttachmentWrite, Layout::ColorAttachment, Stage::None, Access::None, Layout::ShaderRead
        ) }
    );
    CommandList compute = kit.list(QueueType::Compute, "test.compute");
    compute.setPipeline(fetch);
    compute.pushRoot(
        FetchRoot{ .view = static_cast<uint32_t>(view),
                   .sampler = static_cast<uint32_t>(oracles.nearest()),
                   .comparison = 0,
                   .pad = 0,
                   .output = kit.device.address(output) }
    );
    compute.dispatch(1, 1, 1);
    // The compute submission waits on the value the graphics submission signals.
    kit.run(std::array{ graphics, compute });
    CHECK(kit.readBuffer(output, QueueType::Compute) == bytesOf(std::span<const float>(kSolid)));
}
