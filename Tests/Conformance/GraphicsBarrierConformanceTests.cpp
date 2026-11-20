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

    // 64 MiB through 2^22 threads. A pass that pulled the tail without a barrier never overtook the kernel at 16 MiB, and
    // overtook it once in seven runs at 64 MiB, which later runs did not reproduce: this case is one of those that check the
    // lowering.
    constexpr uint32_t kCopyElements = uint32_t{ 1 } << 22;

    constexpr uint64_t kCopyBytes = uint64_t{ kCopyElements } * 16;

    // As `PullOracle.slang` declares it.
    constexpr uint32_t kCopyGroup = 256;

    // What the consumers read: records, a count or positions.
    constexpr uint64_t kTail = 4096;

    constexpr uint64_t kTailOffset = kCopyBytes - kTail;

    constexpr uint64_t kCountOffset = kCopyBytes - 16;

    constexpr uint32_t kBigEdge = 2048;

    // 16 times 2^22 fragments. A depth producer of this size is overtaken by a dispatch without its barrier; 128 triangles
    // made no pass consumer overtake a pass.
    constexpr uint32_t kLayers = 16;

    // The draw and mesh oracles place their triangles on a grid of this edge.
    constexpr uint32_t kEdge = 256;

    // What the solid and pull fragment entries write, exact in every format the cases use but `RGBA8Unorm`.
    constexpr std::array<float, 4> kSolid{ 0.25f, 0.5f, 0.75f, 1.0f };

    // A producer's target holds this before the producer runs, so a consumer that overtakes it reads another value.
    constexpr ClearColor kStale{ 2.0f, 4.0f, 8.0f, 16.0f };

    constexpr Viewport kOracleViewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) };
    constexpr Rect2D kOracleRect{ 0, 0, kEdge, kEdge };

    constexpr Rect2D kBigRect{ 0, 0, kBigEdge, kBigEdge };

    struct CopyRoot
    {
        Addr source;
        Addr destination;
        uint32_t count;
        uint32_t pad;
    };

    struct PullRoot
    {
        Addr vertices;
    };

    struct FragmentRoot
    {
        uint32_t view;
        uint32_t sampler;
        uint32_t pad0;
        uint32_t pad1;
    };

    struct FetchRoot
    {
        uint32_t view;
        uint32_t sampler;
        uint32_t comparison;
        uint32_t pad;
        Addr output;
    };

    struct WriteRoot
    {
        uint32_t view;
        uint32_t pad0;
        uint32_t pad1;
        uint32_t pad2;
        std::array<float, 4> left;
        std::array<float, 4> right;
    };

    // `value` is zero or a normal half-precision value held exactly.
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

    std::array<std::byte, 8> texelOf(const std::array<float, 4>& value)
    {
        std::array<uint16_t, 4> halves{};
        for (size_t channel = 0; channel < 4; ++channel)
        {
            halves[channel] = halfOf(value[channel]);
        }
        return std::bit_cast<std::array<std::byte, 8>>(halves);
    }

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

    template<class T, size_t N> std::vector<std::byte> bytesOf(std::span<T, N> values)
    {
        const std::span<const std::byte> bytes = std::as_bytes(values);
        return { bytes.begin(), bytes.end() };
    }

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

    struct OracleImage
    {
        void set(uint32_t cell, const std::array<float, 4>& value)
        {
            const std::array<std::byte, 8> texel = texelOf(value);
            std::memcpy(bytes.data() + size_t{ cell } * texel.size(), texel.data(), texel.size());
        }

        std::vector<std::byte> bytes = std::vector<std::byte>(size_t{ kEdge } * kEdge * 8);
    };

    ShaderLibrary loadLibrary(Device device, std::string_view name)
    {
        const std::vector<std::byte> blob = test::shaderLibraryFile(name);
        return *device.loadShaderLibrary(blob, name);
    }

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

    Pipeline computeOf(Device device, ShaderLibrary library, std::string_view entry)
    {
        return *device.createPipeline(ComputePipelineDesc{ .compute = { library, entry }, .label = "test.compute" });
    }

    Texture textureOf(Device device, uint32_t edge, Format format, TextureUsage usage, uint32_t samples = 1)
    {
        return *device.createTexture(TextureDesc{ .format = format, .extent = { edge, edge, 1 }, .samples = samples, .usage = usage, .label = "test.texture" });
    }

    // The clear gives the texture a value that a consumer overtaking its producer would read.
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

    BufferBarrier computeWritten(Buffer buffer, Stage dstStage, Access dstAccess)
    {
        return BufferBarrier{
            .buffer = buffer, .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = dstStage, .dstAccess = dstAccess
        };
    }

    struct Oracles
    {
        explicit Oracles(test::GpuKit& created)
            : kit(created), draw(loadLibrary(created.device, "DrawOracle")), pull(loadLibrary(created.device, "PullOracle")),
              fragment(loadLibrary(created.device, "AbiFragment")), copy(computeOf(created.device, pull, "copyMain"))
        {
        }

        Pipeline solid(Format format, uint32_t samples = 1) const
        {
            const std::array targets{ ColorTarget{ .format = format } };
            return graphicsOf(
                kit.device,
                GraphicsPipelineDesc{ .vertex = { draw, "fullscreenVertexMain" }, .fragment = { draw, "solidFragmentMain" }, .samples = samples },
                targets
            );
        }

        Pipeline oracle(Format format = Format::RGBA16Float) const
        {
            const std::array targets{ ColorTarget{ .format = format } };
            return graphicsOf(kit.device, GraphicsPipelineDesc{ .vertex = { draw, "oracleVertexMain" }, .fragment = { draw, "oracleFragmentMain" } }, targets);
        }

        Pipeline pulled() const
        {
            const std::array targets{ ColorTarget{ .format = Format::RGBA16Float } };
            return graphicsOf(kit.device, GraphicsPipelineDesc{ .vertex = { pull, "pullVertexMain" }, .fragment = { pull, "pullFragmentMain" } }, targets);
        }

        Pipeline sampled() const
        {
            const std::array targets{ ColorTarget{ .format = Format::RGBA16Float } };
            return graphicsOf(kit.device, GraphicsPipelineDesc{ .vertex = { draw, "oracleVertexMain" }, .fragment = { fragment, "fragmentMain" } }, targets);
        }

        SamplerIndex nearest() const
        {
            return *kit.device.createSampler(
                SamplerDesc{
                    .filter = SamplerFilter::Nearest, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge, .label = "test.nearest" }
            );
        }

        void copyOn(CommandList list, Addr source, Addr destination, uint32_t count) const
        {
            list.setPipeline(copy);
            list.pushRoot(CopyRoot{ .source = source, .destination = destination, .count = count, .pad = 0 });
            list.dispatch((count + kCopyGroup - 1) / kCopyGroup, 1, 1);
        }

        void produceOn(CommandList list, Buffer source, Buffer destination) const
        {
            copyOn(list, kit.device.address(source), kit.device.address(destination), kCopyElements);
        }

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

        void sampleOn(CommandList list, Texture target, Pipeline pipeline, Texture view) const
        {
            const ViewIndex index = *kit.device.createView(view, {});
            drawOn(list, target, pipeline, FragmentRoot{ static_cast<uint32_t>(index), static_cast<uint32_t>(nearest()), 0, 0 });
        }

        Texture target() const
        {
            const Texture texture = textureOf(kit.device, kEdge, Format::RGBA16Float, TextureUsage::RenderTarget);
            prepare(kit, texture, Layout::ColorAttachment);
            return texture;
        }

        Texture bigTarget(TextureUsage usage = TextureUsage::None) const
        {
            const Texture texture = textureOf(kit.device, kBigEdge, Format::RGBA16Float, TextureUsage::RenderTarget | usage);
            const ClearValue stale{ .color = kStale };
            prepare(kit, texture, Layout::ColorAttachment, &stale);
            return texture;
        }

        std::vector<std::byte> read(Texture texture) const
        {
            CommandList list = kit.list(QueueType::Graphics, "test.toCopy");
            test::transition(list, texture, Layout::ColorAttachment, Layout::CopySrc);
            kit.run(std::array{ list });
            return kit.readTexture(texture, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, Format::RGBA16Float);
        }

        test::GpuKit& kit;
        ShaderLibrary draw;
        ShaderLibrary pull;
        ShaderLibrary fragment;
        Pipeline copy;
    };

    Buffer sourceOf(test::GpuKit& kit, std::span<const std::byte> tail, uint32_t count = 0)
    {
        std::vector<std::byte> bytes(kCopyBytes, std::byte{ 0 });
        REQUIRE(tail.size() <= kTail - 16);
        std::memcpy(bytes.data() + kTailOffset, tail.data(), tail.size());
        std::memcpy(bytes.data() + kCountOffset, &count, sizeof(count));
        return kit.buffer(kCopyBytes, Memory::Upload, BufferUsage::None, bytes);
    }

    Buffer destinationOf(test::GpuKit& kit, BufferUsage usage = BufferUsage::None)
    {
        const std::vector<std::byte> zeroes(kCopyBytes, std::byte{ 0 });
        return kit.buffer(kCopyBytes, Memory::Gpu, BufferUsage::ShaderWrite | usage, zeroes);
    }

    constexpr std::array<std::array<float, 4>, 3> kFullscreen{ std::array{ -1.0f, -1.0f, 0.0f, 1.0f },
                                                               std::array{ 3.0f, -1.0f, 0.0f, 1.0f },
                                                               std::array{ -1.0f, 3.0f, 0.0f, 1.0f } };

    OracleImage solidImage()
    {
        OracleImage image;
        for (uint32_t cell = 0; cell < kEdge * kEdge; ++cell)
        {
            image.set(cell, kSolid);
        }
        return image;
    }

    OracleImage firstPixel(const std::array<float, 4>& value)
    {
        OracleImage image;
        image.set(0, value);
        return image;
    }

    // Only the barrier the producer still owes at its end orders the two lists.
    void submitInOrder(test::GpuKit& kit, CommandList producer, CommandList consumer, bool two)
    {
        producer.end();
        consumer.end();
        const Fence fence = *kit.device.createFence(0, "test.carried");
        const std::array signal{ FenceValue{ fence, 1 } };
        Queue queue = kit.device.queue(QueueType::Graphics);
        if (two)
        {
            queue.submit(std::array{ producer });
            queue.submit(std::array{ consumer }, {}, signal);
        }
        else
        {
            queue.submit(std::array{ producer, consumer }, {}, signal);
        }
        REQUIRE(kit.device.wait(signal[0], UINT64_MAX));
        kit.device.destroy(fence);
    }

    // A backend may lower the barrier a list owes at its end differently once a counted draw has run in an earlier frame
    // (chapter 09's Metal 4 row), so a carry case runs with and without this first. The zeroed records draw nothing.
    void runCountedFrame(test::GpuKit& kit, Pipeline counted, Texture target, Buffer records)
    {
        CommandList list = kit.list(QueueType::Graphics, "test.counted");
        const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kOracleRect, .label = "test.counted" });
        list.setPipeline(counted);
        list.setViewport(kOracleViewport);
        list.setScissor(kOracleRect);
        list.drawIndirect(kit.device.address(records), 64);
        list.endRenderPass();
        kit.run(std::array{ list });
        kit.nextFrame();
    }

    void checkImage(const std::vector<std::byte>& actual, const OracleImage& expected)
    {
        const std::string difference = differences(actual, expected.bytes, 8, kEdge);
        INFO(difference);
        CHECK(difference.empty());
    }

    Texture storageTarget(test::GpuKit& kit)
    {
        const Texture texture = textureOf(kit.device, kBigEdge, Format::RGBA16Float, TextureUsage::Storage | TextureUsage::Sampled);
        const ClearValue stale{ .color = kStale };
        prepare(kit, texture, Layout::ShaderWrite, &stale);
        return texture;
    }

    void storeOn(test::GpuKit& kit, CommandList list, Texture texture)
    {
        const ShaderLibrary library = loadLibrary(kit.device, "FormatOracle");
        const Pipeline store = computeOf(kit.device, library, "storeTexelsMain");
        const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{ .usage = TextureUsage::Storage });
        list.setPipeline(store);
        list.pushRoot(WriteRoot{ static_cast<uint32_t>(view), 0, 0, 0, kSolid, kSolid });
        list.dispatch(kBigEdge / 4, kBigEdge / 4, 1);
    }

    void fetchOn(test::GpuKit& kit, CommandList list, std::string_view entry, ViewIndex view, SamplerIndex sampler, Buffer output)
    {
        const ShaderLibrary textures = loadLibrary(kit.device, "AbiTextures");
        list.setPipeline(computeOf(kit.device, textures, entry));
        list.pushRoot(
            FetchRoot{ .view = static_cast<uint32_t>(view),
                       .sampler = static_cast<uint32_t>(sampler),
                       .comparison = 0,
                       .pad = 0,
                       .output = kit.device.address(output) }
        );
        list.dispatch(1, 1, 1);
    }
} // namespace

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

TEST_CASE("a compute shader's storage-texture writes are loaded by the next dispatch through a storage view", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Texture texture = storageTarget(kit);
    const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{ .usage = TextureUsage::Storage });
    const std::vector<std::byte> zeroes(16, std::byte{ 0 });
    const Buffer output = kit.buffer(16, Memory::Gpu, BufferUsage::ShaderWrite, zeroes);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    storeOn(kit, list, texture);
    list.barrier(
        {},
        {},
        std::array{ textureBarrier(
            texture,
            Stage::ComputeShader,
            Access::ShaderWrite,
            Layout::ShaderWrite,
            Stage::ComputeShader,
            Access::ShaderRead | Access::ShaderWrite,
            Layout::ShaderWrite
        ) }
    );
    fetchOn(kit, list, "rwTexture2DMain", view, SamplerIndex{}, output);
    kit.run(std::array{ list });
    CHECK(kit.readBuffer(output) == bytesOf(std::span<const float>(kSolid)));
}

TEST_CASE("a compute shader's storage-texture writes are sampled by the next dispatch", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Texture texture = storageTarget(kit);
    const ViewIndex view = *kit.device.createView(texture, {});
    const std::vector<std::byte> zeroes(16, std::byte{ 0 });
    const Buffer output = kit.buffer(16, Memory::Gpu, BufferUsage::ShaderWrite, zeroes);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    storeOn(kit, list, texture);
    list.barrier(
        {},
        {},
        std::array{ textureBarrier(
            texture, Stage::ComputeShader, Access::ShaderWrite, Layout::ShaderWrite, Stage::ComputeShader, Access::ShaderRead, Layout::ShaderRead
        ) }
    );
    fetchOn(kit, list, "texture2DMain", view, oracles.nearest(), output);
    kit.run(std::array{ list });
    CHECK(kit.readBuffer(output) == bytesOf(std::span<const float>(kSolid)));
}

TEST_CASE("a compute shader's storage-texture writes are sampled by the next pass's fragment stage", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline sampled = oracles.sampled();
    const Texture texture = storageTarget(kit);
    const Texture target = oracles.target();
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    storeOn(kit, list, texture);
    list.barrier(
        {},
        {},
        std::array{ textureBarrier(
            texture, Stage::ComputeShader, Access::ShaderWrite, Layout::ShaderWrite, Stage::FragmentShader, Access::ShaderRead, Layout::ShaderRead
        ) }
    );
    oracles.sampleOn(list, target, sampled, texture);
    kit.run(std::array{ list });
    checkImage(oracles.read(target), firstPixel(kSolid));
}

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
    test::transition(list, texture, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    std::fill(texels.begin(), texels.begin() + 4, std::byte{ 0xFF });
    const std::vector<std::byte> actual = kit.readTexture(texture, TextureRegion{ .extent = { kBigEdge, kBigEdge, 1 } }, Format::RGBA8Unorm);
    const std::string difference = differences(actual, texels, 4, kBigEdge);
    INFO(difference);
    CHECK(difference.empty());
}

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

TEST_CASE("a pass's closing batch orders the next list's dispatch on the queue, whichever list is recorded first", "[conformance]")
{
    test::requireGpu();
    // Covered: a batch before the pass names its depth stage as a source, so the pass's encoder carries the closing batch.
    // Otherwise a trailing encoder does. After a counted draw in an earlier frame (`fenced`) the dispatch waits on the
    // carry fence, and a counted draw's kernels before it do not; before, the carried barrier holds the dispatch stage.
    const bool covered = GENERATE(false, true);
    const bool consumerFirst = GENERATE(false, true);
    const bool two = GENERATE(false, true);
    const bool kernels = GENERATE(false, true);
    const bool fenced = GENERATE(false, true);
    CAPTURE(covered, consumerFirst, two, kernels, fenced);
    test::GpuKit kit;
    const Oracles oracles(kit);
    const DepthStencilState write{ .depthTest = true, .depthWrite = true, .depthCompare = CompareOp::Always };
    const Pipeline depthOnly = graphicsOf(
        kit.device, GraphicsPipelineDesc{ .vertex = { oracles.draw, "fullscreenVertexMain" }, .depthStencil = write, .depthFormat = Format::D32Float }, {}
    );
    const ShaderLibrary textures = loadLibrary(kit.device, "AbiTextures");
    const Pipeline compare = computeOf(kit.device, textures, "compareMain");
    const Pipeline counted = oracles.oracle();
    const Texture depth = textureOf(kit.device, kBigEdge, Format::D32Float, TextureUsage::DepthStencil | TextureUsage::Sampled);
    // compareMain passes when its reference, 0.5, is at most the stored depth: only the draws' 0.75 passes.
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
    // Records that draw nothing, so the counted draw only runs its kernels.
    const std::vector<std::byte> empty(64 * sizeof(DrawIndirectArgs), std::byte{ 0 });
    const Buffer records = kit.buffer(empty.size(), Memory::Gpu, BufferUsage::IndirectArgs, empty);
    const Texture target = oracles.target();
    if (fenced)
    {
        runCountedFrame(kit, counted, target, records);
    }
    CommandList producer = kit.list(QueueType::Graphics, "test.producer");
    CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
    const auto recordProducer = [&] {
        if (covered)
        {
            producer.barrier(
                {},
                {},
                std::array{ textureBarrier(
                    depth,
                    Stage::DepthStencil,
                    Access::DepthStencilWrite,
                    Layout::DepthStencilWrite,
                    Stage::DepthStencil,
                    Access::DepthStencilRead | Access::DepthStencilWrite,
                    Layout::DepthStencilWrite
                ) }
            );
        }
        producer.beginRenderPass(
            RenderPassDesc{
                .depth = DepthAttachment{ .texture = depth, .load = LoadOp::Clear, .clearDepth = 0.0f }, .area = kBigRect, .label = "test.producer" }
        );
        producer.setPipeline(depthOnly);
        // The viewport maps the triangle's clip-space depth of 0.5 to 0.75.
        producer.setViewport(Viewport{ .width = static_cast<float>(kBigEdge), .height = static_cast<float>(kBigEdge), .minDepth = 0.5f, .maxDepth = 1.0f });
        producer.setScissor(kBigRect);
        producer.draw(3, kLayers);
        producer.endRenderPass();
        producer.barrier(
            {},
            {},
            std::array{ textureBarrier(
                depth,
                Stage::DepthStencil,
                Access::DepthStencilWrite,
                Layout::DepthStencilWrite,
                Stage::ComputeShader,
                Access::ShaderRead,
                Layout::DepthStencilRead
            ) }
        );
    };
    const auto recordConsumer = [&] {
        if (kernels)
        {
            const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
            consumer.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kOracleRect, .label = "test.kernels" });
            consumer.setPipeline(counted);
            consumer.setViewport(kOracleViewport);
            consumer.setScissor(kOracleRect);
            consumer.drawIndirect(kit.device.address(records), 64);
            consumer.endRenderPass();
        }
        consumer.setPipeline(compare);
        consumer.pushRoot(
            FetchRoot{ .view = static_cast<uint32_t>(view),
                       .sampler = 0,
                       .comparison = static_cast<uint32_t>(comparison),
                       .pad = 0,
                       .output = kit.device.address(output) }
        );
        consumer.dispatch(1, 1, 1);
    };
    if (consumerFirst)
    {
        recordConsumer();
        recordProducer();
    }
    else
    {
        recordProducer();
        recordConsumer();
    }
    submitInOrder(kit, producer, consumer, two);
    const std::array<float, 4> passed{ 1.0f, 0.0f, 0.0f, 0.0f };
    CHECK(kit.readBuffer(output) == bytesOf(std::span<const float>(passed)));
}

TEST_CASE("a pass's closing batch orders the next list's copy and sampling pass on the queue", "[conformance]")
{
    test::requireGpu();
    const bool covered = GENERATE(false, true);
    const bool consumerFirst = GENERATE(false, true);
    CAPTURE(covered, consumerFirst);
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline solid = oracles.solid(Format::RGBA16Float);
    const Pipeline sampled = oracles.sampled();
    const Texture produced = oracles.bigTarget(TextureUsage::Sampled);
    constexpr uint32_t kRow = kBigEdge * 8;
    constexpr uint32_t kRows = 4;
    const Buffer readback = kit.buffer(uint64_t{ kRow } * kRows, Memory::Readback);
    const Texture target = oracles.target();
    CommandList producer = kit.list(QueueType::Graphics, "test.producer");
    CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
    const auto recordProducer = [&](Stage stage, Access access, Layout layout) {
        if (covered)
        {
            // The pass's encoder then waits for every earlier color write, so it carries the closing batch itself.
            producer.barrier(
                {},
                {},
                std::array{ textureBarrier(
                    produced,
                    Stage::ColorAttachment,
                    Access::ColorAttachmentWrite,
                    Layout::ColorAttachment,
                    Stage::ColorAttachment,
                    Access::ColorAttachmentWrite,
                    Layout::ColorAttachment
                ) }
            );
        }
        Oracles::renderOn(producer, produced, solid);
        producer.barrier(
            {}, {}, std::array{ textureBarrier(produced, Stage::ColorAttachment, Access::ColorAttachmentWrite, Layout::ColorAttachment, stage, access, layout) }
        );
    };
    SECTION("a copy")
    {
        const auto recordConsumer = [&] {
            consumer.copyTextureToBuffer(
                produced,
                TextureRegion{ .offset = { 0, kBigEdge - kRows, 0 }, .extent = { kBigEdge, kRows, 1 } },
                readback,
                BufferTextureLayout{ .rowPitch = kRow }
            );
        };
        if (consumerFirst)
        {
            recordConsumer();
            recordProducer(Stage::Copy, Access::CopyRead, Layout::CopySrc);
        }
        else
        {
            recordProducer(Stage::Copy, Access::CopyRead, Layout::CopySrc);
            recordConsumer();
        }
        submitInOrder(kit, producer, consumer, false);
        const std::string difference = differences(kit.read(readback), texelsOf(uint64_t{ kBigEdge } * kRows, kSolid), 8, kBigEdge);
        INFO(difference);
        CHECK(difference.empty());
    }
    SECTION("a pass that samples")
    {
        if (consumerFirst)
        {
            oracles.sampleOn(consumer, target, sampled, produced);
            recordProducer(Stage::FragmentShader, Access::ShaderRead, Layout::ShaderRead);
        }
        else
        {
            recordProducer(Stage::FragmentShader, Access::ShaderRead, Layout::ShaderRead);
            oracles.sampleOn(consumer, target, sampled, produced);
        }
        submitInOrder(kit, producer, consumer, false);
        checkImage(oracles.read(target), firstPixel(kSolid));
    }
}

TEST_CASE("a closing batch into IndirectArgs orders the next list's counted draw on the queue", "[conformance]")
{
    test::requireGpu();
    // Covered: a batch before the producer names the compute stage as a source, so the dispatch's encoder carries the
    // closing batch with a barrier that holds the counted draw's kernels back. Otherwise a trailing encoder does.
    const bool covered = GENERATE(false, true);
    const bool consumerFirst = GENERATE(false, true);
    CAPTURE(covered, consumerFirst);
    constexpr uint32_t kMaxCount = 64;
    test::GpuKit kit;
    const Oracles oracles(kit);
    const Pipeline pipeline = oracles.oracle();
    std::vector<DrawIndirectArgs> records;
    for (uint32_t i = 0; i < kMaxCount; ++i)
    {
        records.push_back(DrawIndirectArgs{ .drawId = 2 * i + 5, .vertexCount = 3, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0 });
    }
    constexpr uint32_t kCount = 37;
    const Buffer source = sourceOf(kit, bytesOf(std::span<const DrawIndirectArgs>(records)), kCount);
    // The records and the count start as zeroes, which draw nothing.
    const Buffer args = destinationOf(kit, BufferUsage::IndirectArgs);
    const Texture target = oracles.target();
    CommandList producer = kit.list(QueueType::Graphics, "test.producer");
    CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
    const auto recordProducer = [&] {
        if (covered)
        {
            producer.barrier(
                std::array{ GlobalBarrier{ .srcStage = Stage::ComputeShader,
                                           .srcAccess = Access::ShaderWrite,
                                           .dstStage = Stage::ComputeShader,
                                           .dstAccess = Access::ShaderRead | Access::ShaderWrite } }
            );
        }
        oracles.produceOn(producer, source, args);
        producer.barrier({}, std::array{ computeWritten(args, Stage::IndirectArgs, Access::IndirectArgs) });
    };
    const auto recordConsumer = [&] {
        const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
        consumer.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kOracleRect, .label = "test.consumer" });
        consumer.setPipeline(pipeline);
        consumer.setViewport(kOracleViewport);
        consumer.setScissor(kOracleRect);
        consumer.drawIndirect(kit.device.address(args, kTailOffset), kMaxCount, kit.device.address(args, kCountOffset));
        consumer.endRenderPass();
    };
    if (consumerFirst)
    {
        recordConsumer();
        recordProducer();
    }
    else
    {
        recordProducer();
        recordConsumer();
    }
    submitInOrder(kit, producer, consumer, false);
    OracleImage expected;
    for (uint32_t i = 0; i < kCount; ++i)
    {
        expected.set(records[i].drawId, { static_cast<float>(records[i].drawId + 1), 1.0f, 1.0f, 1.0f });
    }
    checkImage(oracles.read(target), expected);
}

TEST_CASE("two lists that carry barriers in a row both order a later list's dispatch", "[conformance]")
{
    test::requireGpu();
    // List A carries its depth batch on its pass's encoder; list B renders elsewhere and carries its own batch, on its
    // encoder or a trailing one; list C's dispatch reads A's depth, so after a counted draw in an earlier frame (`fenced`)
    // its wait must see A's update behind B's.
    const bool coveredB = GENERATE(false, true);
    const bool fenced = GENERATE(false, true);
    CAPTURE(coveredB, fenced);
    test::GpuKit kit;
    const Oracles oracles(kit);
    if (fenced)
    {
        const std::vector<std::byte> empty(64 * sizeof(DrawIndirectArgs), std::byte{ 0 });
        runCountedFrame(kit, oracles.oracle(), oracles.target(), kit.buffer(empty.size(), Memory::Gpu, BufferUsage::IndirectArgs, empty));
    }
    const DepthStencilState write{ .depthTest = true, .depthWrite = true, .depthCompare = CompareOp::Always };
    const Pipeline depthOnly = graphicsOf(
        kit.device, GraphicsPipelineDesc{ .vertex = { oracles.draw, "fullscreenVertexMain" }, .depthStencil = write, .depthFormat = Format::D32Float }, {}
    );
    const Pipeline solid = oracles.solid(Format::RGBA16Float);
    const ShaderLibrary textures = loadLibrary(kit.device, "AbiTextures");
    const Pipeline compare = computeOf(kit.device, textures, "compareMain");
    const Texture depth = textureOf(kit.device, kBigEdge, Format::D32Float, TextureUsage::DepthStencil | TextureUsage::Sampled);
    // compareMain passes when its reference, 0.5, is at most the stored depth: only the draws' 0.75 passes.
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
    const Texture other = textureOf(kit.device, kEdge, Format::RGBA16Float, TextureUsage::RenderTarget | TextureUsage::Sampled);
    prepare(kit, other, Layout::ColorAttachment);
    CommandList a = kit.list(QueueType::Graphics, "test.a");
    CommandList b = kit.list(QueueType::Graphics, "test.b");
    CommandList c = kit.list(QueueType::Graphics, "test.c");
    a.barrier(
        {},
        {},
        std::array{ textureBarrier(
            depth,
            Stage::DepthStencil,
            Access::DepthStencilWrite,
            Layout::DepthStencilWrite,
            Stage::DepthStencil,
            Access::DepthStencilRead | Access::DepthStencilWrite,
            Layout::DepthStencilWrite
        ) }
    );
    a.beginRenderPass(
        RenderPassDesc{ .depth = DepthAttachment{ .texture = depth, .load = LoadOp::Clear, .clearDepth = 0.0f }, .area = kBigRect, .label = "test.a" }
    );
    a.setPipeline(depthOnly);
    // The viewport maps the triangle's clip-space depth of 0.5 to 0.75.
    a.setViewport(Viewport{ .width = static_cast<float>(kBigEdge), .height = static_cast<float>(kBigEdge), .minDepth = 0.5f, .maxDepth = 1.0f });
    a.setScissor(kBigRect);
    a.draw(3, kLayers);
    a.endRenderPass();
    a.barrier(
        {},
        {},
        std::array{ textureBarrier(
            depth, Stage::DepthStencil, Access::DepthStencilWrite, Layout::DepthStencilWrite, Stage::ComputeShader, Access::ShaderRead, Layout::DepthStencilRead
        ) }
    );
    if (coveredB)
    {
        b.barrier(
            {},
            {},
            std::array{ textureBarrier(
                other,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite,
                Layout::ColorAttachment,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite,
                Layout::ColorAttachment
            ) }
        );
    }
    Oracles::renderOn(b, other, solid, {}, kEdge);
    b.barrier(
        {},
        {},
        std::array{ textureBarrier(
            other, Stage::ColorAttachment, Access::ColorAttachmentWrite, Layout::ColorAttachment, Stage::ComputeShader, Access::ShaderRead, Layout::ShaderRead
        ) }
    );
    c.setPipeline(compare);
    c.pushRoot(
        FetchRoot{
            .view = static_cast<uint32_t>(view), .sampler = 0, .comparison = static_cast<uint32_t>(comparison), .pad = 0, .output = kit.device.address(output) }
    );
    c.dispatch(1, 1, 1);
    // One submission, so only the carried barriers order the lists.
    kit.run(std::array{ a, b, c });
    const std::array<float, 4> passed{ 1.0f, 0.0f, 0.0f, 0.0f };
    CHECK(kit.readBuffer(output) == bytesOf(std::span<const float>(passed)));
}

TEST_CASE("a barrier on a compute encoder that ran only copies orders the dispatch after it", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    // The dispatch reads the tail of a 64 MiB blit's destination, so a dispatch the barrier left unordered would read zeroes.
    const std::vector<std::byte> tailA = test::pattern(kTail - 16, 41);
    const std::vector<std::byte> tailB = test::pattern(kTail - 16, 42);
    const Buffer sourceA = sourceOf(kit, tailA);
    const Buffer sourceB = sourceOf(kit, tailB);
    const Buffer destinationA = destinationOf(kit);
    const Buffer destinationB = destinationOf(kit);
    const std::vector<std::byte> zeroes(kTail, std::byte{ 0 });
    const Buffer outA = kit.buffer(kTail, Memory::Gpu, BufferUsage::ShaderWrite, zeroes);
    const Buffer outB = kit.buffer(kTail, Memory::Gpu, BufferUsage::ShaderWrite, zeroes);
    const auto copied = [](Buffer buffer) {
        return BufferBarrier{
            .buffer = buffer, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderRead
        };
    };
    const auto consume = [&](CommandList list, Buffer destination, Buffer out) {
        oracles.copyOn(list, kit.device.address(destination, kTailOffset), kit.device.address(out), static_cast<uint32_t>(kTail / 16));
    };
    const auto head = [&](Buffer out, size_t size) {
        const std::vector<std::byte> bytes = kit.readBuffer(out);
        return std::vector<std::byte>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
    };
    SECTION("the copy in the same encoder")
    {
        CommandList list = kit.list(QueueType::Graphics, "test.list");
        list.copyBuffer(sourceA, 0, destinationA, 0, kCopyBytes);
        list.barrier({}, std::array{ copied(destinationA) });
        consume(list, destinationA, outA);
        kit.run(std::array{ list });
        CHECK(head(outA, tailA.size()) == tailA);
    }
    SECTION("the copy in an earlier list, the encoder holding an unrelated fill")
    {
        CommandList producer = kit.list(QueueType::Graphics, "test.producer");
        producer.copyBuffer(sourceA, 0, destinationA, 0, kCopyBytes);
        CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
        consumer.fillBuffer(outB, 0, kTail, 0);
        consumer.barrier({}, std::array{ copied(destinationA) });
        consume(consumer, destinationA, outA);
        kit.run(std::array{ producer, consumer });
        CHECK(head(outA, tailA.size()) == tailA);
    }
    SECTION("copies in an earlier list and in the same encoder")
    {
        CommandList producer = kit.list(QueueType::Graphics, "test.producer");
        producer.copyBuffer(sourceA, 0, destinationA, 0, kCopyBytes);
        CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
        consumer.copyBuffer(sourceB, 0, destinationB, 0, kCopyBytes);
        consumer.barrier({}, std::array{ copied(destinationA), copied(destinationB) });
        consume(consumer, destinationA, outA);
        consume(consumer, destinationB, outB);
        kit.run(std::array{ producer, consumer });
        CHECK(head(outA, tailA.size()) == tailA);
        CHECK(head(outB, tailB.size()) == tailB);
    }
}

TEST_CASE("a barrier an encoder kept and a later command of it spent still orders the next list's dispatch after the encoder's earlier work", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracles oracles(kit);
    // List 1's first encoder opens with a barrier, runs the 64 MiB kernel that writes the destination, keeps a batch for it and
    // runs an unrelated dispatch, which spends the batch; the list then ends with nothing pending. List 2 reads the
    // destination's tail with no barrier of its own, so the producer barrier that ends list 1's encoder is all that orders its
    // dispatch after the kernel. With that barrier left out, the case failed in 199 of 200 runs on the Apple GPU tried.
    const std::vector<std::byte> tail = test::pattern(kTail - 16, 61);
    const Buffer source = sourceOf(kit, tail);
    const Buffer destination = destinationOf(kit);
    const Buffer unrelatedSource = sourceOf(kit, test::pattern(kTail - 16, 62));
    const Buffer unrelatedDestination = destinationOf(kit);
    const std::vector<std::byte> zeroes(kTail, std::byte{ 0 });
    const Buffer out = kit.buffer(kTail, Memory::Gpu, BufferUsage::ShaderWrite, zeroes);
    const GlobalBarrier computeToCompute{ .srcStage = Stage::ComputeShader,
                                          .srcAccess = Access::ShaderWrite,
                                          .dstStage = Stage::ComputeShader,
                                          .dstAccess = Access::ShaderRead | Access::ShaderWrite };
    const auto produced = BufferBarrier{ .buffer = destination,
                                         .srcStage = Stage::ComputeShader,
                                         .srcAccess = Access::ShaderWrite,
                                         .dstStage = Stage::ComputeShader,
                                         .dstAccess = Access::ShaderRead };
    CommandList producer = kit.list(QueueType::Graphics, "test.producer");
    producer.barrier(std::array{ computeToCompute });
    oracles.produceOn(producer, source, destination);
    producer.barrier({}, std::array{ produced });
    oracles.copyOn(producer, kit.device.address(unrelatedSource, kTailOffset), kit.device.address(unrelatedDestination), static_cast<uint32_t>(kTail / 16));
    CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
    oracles.copyOn(consumer, kit.device.address(destination, kTailOffset), kit.device.address(out), static_cast<uint32_t>(kTail / 16));
    kit.run(std::array{ producer, consumer });
    const std::vector<std::byte> read = kit.readBuffer(out);
    CHECK(std::vector<std::byte>(read.begin(), read.begin() + static_cast<std::ptrdiff_t>(tail.size())) == tail);
}

namespace {
    // As `FragmentStoreOracle.slang` declares it.
    struct StoreRoot
    {
        Addr destination;
        uint32_t iterations;
        uint32_t zero;
        uint32_t edge;
        std::array<uint32_t, 4> pad;
    };

    // Each fragment of one fullscreen draw over 2^22 pixels spins this many steps before its store, so each record has one
    // writer and the pass runs for some milliseconds, long enough for a copy that is not ordered after it to run first.
    constexpr uint32_t kSpin = 8192;

    // The records `storeFragmentMain` writes for the pixels of a `kBigEdge` target that a copy of its buffer has not caught.
    uint64_t staleRecords(std::span<const std::byte> bytes)
    {
        REQUIRE(bytes.size() == kCopyBytes);
        uint64_t stale = 0;
        for (uint32_t cell = 0; cell < kCopyElements; ++cell)
        {
            std::array<uint32_t, 4> record{};
            std::memcpy(record.data(), bytes.data() + size_t{ cell } * sizeof(record), sizeof(record));
            if (record != std::array<uint32_t, 4>{ cell, ~cell, 0xF00DF00Du, 0u })
            {
                ++stale;
            }
        }
        return stale;
    }
} // namespace

TEST_CASE("a list that ends owing a carry and a kept batch orders the next list's dispatch after both", "[conformance]")
{
    test::requireGpu();
    // The first encoder opens with no barrier and runs a 64 MiB blit that writes the destination. A barrier from the copy
    // to the dispatch stage follows; the encoder keeps it as its opening barrier and holds it back for its own later commands,
    // and the unrelated dispatch after it spends it. A second batch, from dispatches to dispatches, is still held at the end
    // of the list, which therefore owes a carry for it and, for the kept batch, a producer barrier, which Metal 4 folds into
    // the carry: the kept batch's blit stage is not among the carry's sources. The next list reads the destination's tail with
    // no barrier of its own. After a counted draw in an earlier frame (`fenced`) the carry goes through the fence for the dispatch
    // stage; before it, through a barrier. On the Apple M4 Pro this case passed with the fold made to drop the kept sources too:
    // the next dispatch was still ordered after the blit, so the case checks the order and
    // the unit case of the fold pins the call counts and the kept state, not the source mask.
    const bool fenced = GENERATE(false, true);
    const bool two = GENERATE(false, true);
    CAPTURE(fenced, two);
    test::GpuKit kit;
    const Oracles oracles(kit);
    if (fenced)
    {
        const std::vector<std::byte> empty(64 * sizeof(DrawIndirectArgs), std::byte{ 0 });
        runCountedFrame(kit, oracles.oracle(), oracles.target(), kit.buffer(empty.size(), Memory::Gpu, BufferUsage::IndirectArgs, empty));
    }
    const std::vector<std::byte> tail = test::pattern(kTail - 16, 71);
    const Buffer source = sourceOf(kit, tail);
    const Buffer destination = destinationOf(kit);
    const Buffer unrelatedSource = sourceOf(kit, test::pattern(kTail - 16, 72));
    const Buffer unrelatedDestination = destinationOf(kit);
    const std::vector<std::byte> zeroes(kTail, std::byte{ 0 });
    const Buffer out = kit.buffer(kTail, Memory::Gpu, BufferUsage::ShaderWrite, zeroes);
    CommandList producer = kit.list(QueueType::Graphics, "test.producer");
    producer.copyBuffer(source, 0, destination, 0, kCopyBytes);
    producer.barrier(
        {},
        std::array{ BufferBarrier{ .buffer = destination,
                                   .srcStage = Stage::Copy,
                                   .srcAccess = Access::CopyWrite,
                                   .dstStage = Stage::ComputeShader,
                                   .dstAccess = Access::ShaderRead } }
    );
    oracles.copyOn(producer, kit.device.address(unrelatedSource, kTailOffset), kit.device.address(unrelatedDestination), static_cast<uint32_t>(kTail / 16));
    producer.barrier({}, std::array{ computeWritten(unrelatedDestination, Stage::ComputeShader, Access::ShaderRead) });
    CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
    oracles.copyOn(consumer, kit.device.address(destination, kTailOffset), kit.device.address(out), static_cast<uint32_t>(kTail / 16));
    submitInOrder(kit, producer, consumer, two);
    const std::vector<std::byte> read = kit.readBuffer(out);
    CHECK(std::vector<std::byte>(read.begin(), read.begin() + static_cast<std::ptrdiff_t>(tail.size())) == tail);
}

TEST_CASE("a copy after two barriers met before an empty compute encoder's first command follows the fragment work the first named", "[conformance]")
{
    test::requireGpu();
    // A render pass writes a 64 MiB buffer from its fragment stage, a record for each pixel after a spin that keeps the pass
    // running. Then, on the same list, a barrier from the fragment stage to the compute stage, a compute pipeline whose binding
    // opens an empty compute encoder with that barrier as its opening barrier, a barrier from the compute stage to copies and
    // no dispatch between them. Chapter 09's chain orders the copy after the fragment work through the two barriers, though no
    // dispatch is between them, so the copy of the buffer holds every record. Without either barrier the copy read none on the
    // Apple M4 Pro. With the second opening barrier's sources not widened it still read all, so the case checks the order and
    // the unit case of the widening pins the call's masks.
    test::GpuKit kit;
    const Oracles oracles(kit);
    const ShaderLibrary store = loadLibrary(kit.device, "FragmentStoreOracle");
    const std::array targets{ ColorTarget{ .format = Format::RGBA16Float } };
    const Pipeline producing =
        graphicsOf(kit.device, GraphicsPipelineDesc{ .vertex = { oracles.draw, "fullscreenVertexMain" }, .fragment = { store, "storeFragmentMain" } }, targets);
    const Texture target = oracles.bigTarget();
    const Buffer produced = destinationOf(kit);
    const Buffer copied = destinationOf(kit);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kBigRect, .label = "test.producer" });
    list.setPipeline(producing);
    list.setViewport(Viewport{ .width = static_cast<float>(kBigEdge), .height = static_cast<float>(kBigEdge) });
    list.setScissor(kBigRect);
    list.pushRoot(StoreRoot{ .destination = kit.device.address(produced), .iterations = kSpin, .zero = 0, .edge = kBigEdge, .pad = {} });
    list.draw(3);
    list.endRenderPass();
    list.barrier(
        {},
        std::array{ BufferBarrier{ .buffer = produced,
                                   .srcStage = Stage::FragmentShader,
                                   .srcAccess = Access::ShaderWrite,
                                   .dstStage = Stage::ComputeShader,
                                   .dstAccess = Access::ShaderRead } }
    );
    list.setPipeline(oracles.copy);
    list.barrier(
        {},
        std::array{ BufferBarrier{
            .buffer = produced, .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead } }
    );
    list.copyBuffer(produced, 0, copied, 0, kCopyBytes);
    kit.run(std::array{ list });
    CHECK(staleRecords(kit.readBuffer(copied)) == 0);
}

namespace {
    // A chain from the fragment stage to copies through the compute stage, S1 -> D1 then D1 -> D2, with a copy between the two
    // barriers and no dispatch anywhere after the first. The fragment producer stores a record for each pixel of `produced`
    // after a spin; the last copy reads it into `copied`, whose stale records the cases count.
    struct UnrunChain
    {
        explicit UnrunChain(test::GpuKit& created)
            : kit(created), oracles(created), store(loadLibrary(created.device, "FragmentStoreOracle")),
              producing(graphicsOf(
                  created.device,
                  GraphicsPipelineDesc{ .vertex = { oracles.draw, "fullscreenVertexMain" }, .fragment = { store, "storeFragmentMain" } },
                  std::array{ ColorTarget{ .format = Format::RGBA16Float } }
              )),
              target(oracles.bigTarget()), produced(destinationOf(created)), copied(destinationOf(created)),
              unrelatedSource(sourceOf(created, test::pattern(kTail - 16, 73))), unrelatedDestination(destinationOf(created))
        {
        }

        // The render pass that writes `produced`, the barrier from its fragment stage to the compute stage, and a copy
        // of unrelated buffers, which runs no dispatch.
        void begin(CommandList list) const
        {
            const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
            list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kBigRect, .label = "test.producer" });
            list.setPipeline(producing);
            list.setViewport(Viewport{ .width = static_cast<float>(kBigEdge), .height = static_cast<float>(kBigEdge) });
            list.setScissor(kBigRect);
            list.pushRoot(StoreRoot{ .destination = kit.device.address(produced), .iterations = kSpin, .zero = 0, .edge = kBigEdge, .pad = {} });
            list.draw(3);
            list.endRenderPass();
            list.barrier(
                {},
                std::array{ BufferBarrier{ .buffer = produced,
                                           .srcStage = Stage::FragmentShader,
                                           .srcAccess = Access::ShaderWrite,
                                           .dstStage = Stage::ComputeShader,
                                           .dstAccess = Access::ShaderRead } }
            );
            list.copyBuffer(unrelatedSource, 0, unrelatedDestination, 0, kTail);
        }

        // The barrier from the compute stage to copies that extends the chain, and the copy it orders.
        void end(CommandList list) const
        {
            list.barrier(
                {},
                std::array{ BufferBarrier{ .buffer = produced,
                                           .srcStage = Stage::ComputeShader,
                                           .srcAccess = Access::ShaderWrite,
                                           .dstStage = Stage::Copy,
                                           .dstAccess = Access::CopyRead } }
            );
            list.copyBuffer(produced, 0, copied, 0, kCopyBytes);
        }

        uint64_t stale() const
        {
            return staleRecords(kit.readBuffer(copied));
        }

        test::GpuKit& kit;
        Oracles oracles;
        ShaderLibrary store;
        Pipeline producing;
        Texture target;
        Buffer produced;
        Buffer copied;
        Buffer unrelatedSource;
        Buffer unrelatedDestination;
    };
} // namespace

TEST_CASE("a copy after a chain through the compute stage follows the fragment work when the copy between the barriers ends its encoder", "[conformance]")
{
    test::requireGpu();
    // On Metal 4 the copy between the barriers opens an encoder that holds the dispatch stage for the fragment work and runs no
    // dispatch. The second barrier, already pending, ends it, and the unrun stage returns with the fragment source.
    test::GpuKit kit;
    const UnrunChain chain(kit);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    chain.begin(list);
    chain.end(list);
    kit.run(std::array{ list });
    CHECK(chain.stale() == 0);
}

TEST_CASE("a copy after a chain through the compute stage follows the fragment work when a render pass lies between the barriers", "[conformance]")
{
    test::requireGpu();
    // On Metal 4 the empty pass ends the copy's encoder before the second barrier exists, so only the unrun dispatch stage, still
    // pending with the fragment source, carries that source to the last copy.
    test::GpuKit kit;
    const UnrunChain chain(kit);
    const Texture between = chain.oracles.target();
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    chain.begin(list);
    const std::array colors{ ColorAttachment{ .texture = between, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kOracleRect, .label = "test.between" });
    list.endRenderPass();
    chain.end(list);
    kit.run(std::array{ list });
    CHECK(chain.stale() == 0);
}
