#include "GpuTestSupport.h"

#include <chrono>
#include <cstring>

namespace {

//======================================================================================================================
bool channelIs(uint8_t actual, float expected) {
    return expected > 0.5f ? actual > 200 : actual < 55;
}

} // namespace

//======================================================================================================================
// A throwaway aligned upload puts the triangle away from offset zero, exposing lost offset
// arithmetic.
TEST_CASE("uniform ring feeds a draw from a non-zero offset", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto target = (*device)->createTexture({.width = kSize,
                                            .height = kSize,
                                            .format = Format::BGRA8Unorm,
                                            .renderTarget = true,
                                            .cpuReadback = true,
                                            .label = "lmx.test.ringTarget"});
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createGraphicsPipeline({.library = library->get(),
                                                       .vertexEntry = "vertexMain",
                                                       .fragmentEntry = "fragmentMain",
                                                       .colorFormat = Format::BGRA8Unorm,
                                                       .label = "lmx.test.ringPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    const std::array<uint8_t, 100> filler{};

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.uniformUpload"});
    commands.bindPipeline(**pipeline);
    commands.setUniforms(kVertexBufferSlot, filler.data(), filler.size());
    commands.setUniforms(kVertexBufferSlot, kTriangle.data(), sizeof(kTriangle));
    commands.draw(static_cast<uint32_t>(kTriangle.size()));
    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*target)->readback(pixels.data(), pixels.size());

    const Pixel corner = pixelAt(pixels, 2, 2);
    INFO(describe("corner", 2, 2, corner));
    REQUIRE(corner.b == 0);
    REQUIRE(corner.g == 0);
    REQUIRE(corner.r == 0);
    REQUIRE(corner.a == 255);

    const Pixel apex = pixelAt(pixels, 32, 24);
    INFO(describe("apex", 32, 24, apex));
    REQUIRE(apex.r > 128);
    REQUIRE(apex.r > apex.g);
    REQUIRE(apex.r > apex.b);

    const Pixel bottomLeft = pixelAt(pixels, 20, 46);
    INFO(describe("bottom-left", 20, 46, bottomLeft));
    REQUIRE(bottomLeft.g > 128);
    REQUIRE(bottomLeft.g > bottomLeft.r);
    REQUIRE(bottomLeft.g > bottomLeft.b);

    const Pixel bottomRight = pixelAt(pixels, 43, 46);
    INFO(describe("bottom-right", 43, 46, bottomRight));
    REQUIRE(bottomRight.b > 128);
    REQUIRE(bottomRight.b > bottomRight.r);
    REQUIRE(bottomRight.b > bottomRight.g);
}

//======================================================================================================================
// Six drained frames rotate through all three ring slots twice; each image must retain its own
// color.
TEST_CASE("uniform ring keeps per-frame data across slot reuse", "[gpu]") {
    using namespace lmx::rhi;

    constexpr std::array<std::array<float, 3>, 6> kFrameColors = {{
        {1.0f, 0.0f, 0.0f}, // frame 0 -> ring slot 1
        {0.0f, 1.0f, 0.0f}, // frame 1 -> ring slot 2
        {0.0f, 0.0f, 1.0f}, // frame 2 -> ring slot 0
        {1.0f, 1.0f, 0.0f}, // frame 3 -> ring slot 1 again, first frame that waits
        {0.0f, 1.0f, 1.0f}, // frame 4 -> ring slot 2 again
        {1.0f, 0.0f, 1.0f}, // frame 5 -> ring slot 0 again
    }};

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto target = (*device)->createTexture({.width = kSize,
                                            .height = kSize,
                                            .format = Format::BGRA8Unorm,
                                            .renderTarget = true,
                                            .cpuReadback = true,
                                            .label = "lmx.test.rotationTarget"});
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createGraphicsPipeline({.library = library->get(),
                                                       .vertexEntry = "vertexMain",
                                                       .fragmentEntry = "fragmentMain",
                                                       .colorFormat = Format::BGRA8Unorm,
                                                       .label = "lmx.test.rotationPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);

    for (uint32_t frame = 0; frame < kFrameColors.size(); ++frame) {
        const std::array<float, 3>& color = kFrameColors[frame];

        std::array<Vertex, 3> vertices = kTriangle;
        for (Vertex& vertex : vertices) {
            vertex.color[0] = color[0];
            vertex.color[1] = color[1];
            vertex.color[2] = color[2];
        }

        CommandList& commands = (*device)->beginFrame();
        commands.beginRenderPass({.colorTarget = target->get(),
                                  .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                                  .clear = true,
                                  .label = "lmx.test.frameRotation"});
        commands.bindPipeline(**pipeline);
        commands.setUniforms(kVertexBufferSlot, vertices.data(), sizeof(vertices));
        commands.draw(static_cast<uint32_t>(vertices.size()));
        commands.endRenderPass();
        (*device)->endFrame(nullptr);
        (*device)->waitIdle();
        (*target)->readback(pixels.data(), pixels.size());

        const Pixel inside = pixelAt(pixels, 32, 40);
        INFO("frame " + std::to_string(frame) + " expected R=" + std::to_string(color[0]) +
             " G=" + std::to_string(color[1]) + " B=" + std::to_string(color[2]));
        INFO(describe("inside", 32, 40, inside));
        REQUIRE(channelIs(inside.r, color[0]));
        REQUIRE(channelIs(inside.g, color[1]));
        REQUIRE(channelIs(inside.b, color[2]));
        REQUIRE(inside.a == 255);
    }
}

//======================================================================================================================
// Two coplanar draws pin Less testing and depth writes: red must survive the rejected green draw.
TEST_CASE("depth test rejects a coplanar second draw", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto target = (*device)->createTexture({.width = kSize,
                                            .height = kSize,
                                            .format = Format::BGRA8Unorm,
                                            .renderTarget = true,
                                            .cpuReadback = true,
                                            .label = "lmx.test.depthColorTarget"});
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    auto depthTarget = (*device)->createTexture({.width = kSize,
                                                 .height = kSize,
                                                 .format = Format::D32Float,
                                                 .renderTarget = true,
                                                 .label = "lmx.test.depthTarget"});
    INFO(errorOf(depthTarget));
    REQUIRE(depthTarget.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createGraphicsPipeline({.library = library->get(),
                                                       .vertexEntry = "vertexMain",
                                                       .fragmentEntry = "fragmentMain",
                                                       .colorFormat = Format::BGRA8Unorm,
                                                       .depthFormat = Format::D32Float,
                                                       .depthTestEnable = true,
                                                       .depthWriteEnable = true,
                                                       .label = "lmx.test.depthPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    const auto flatTriangle = [](float r, float g, float b) {
        std::array<Vertex, 3> vertices = kTriangle;
        for (Vertex& vertex : vertices) {
            vertex.color[0] = r;
            vertex.color[1] = g;
            vertex.color[2] = b;
        }
        return vertices;
    };
    const std::array<Vertex, 3> first = flatTriangle(1.0f, 0.0f, 0.0f);
    const std::array<Vertex, 3> second = flatTriangle(0.0f, 1.0f, 0.0f);

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .depthTarget = depthTarget->get(),
                              .clearDepth = 1.0f,
                              .label = "lmx.test.depth.first"});
    commands.bindPipeline(**pipeline);
    commands.setUniforms(kVertexBufferSlot, first.data(), sizeof(first));
    commands.draw(static_cast<uint32_t>(first.size()));
    commands.setUniforms(kVertexBufferSlot, second.data(), sizeof(second));
    commands.draw(static_cast<uint32_t>(second.size()));
    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*target)->readback(pixels.data(), pixels.size());

    const Pixel inside = pixelAt(pixels, 32, 40);
    INFO(describe("inside", 32, 40, inside));
    REQUIRE(channelIs(inside.r, 1.0f));
    REQUIRE(channelIs(inside.g, 0.0f));
    REQUIRE(inside.a == 255);

    const Pixel corner = pixelAt(pixels, 2, 2);
    INFO(describe("corner", 2, 2, corner));
    REQUIRE(corner.r == 0);
    REQUIRE(corner.g == 0);
    REQUIRE(corner.b == 0);
    REQUIRE(corner.a == 255);

    CommandList& secondFrame = (*device)->beginFrame();
    secondFrame.beginRenderPass({.colorTarget = target->get(),
                                 .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                                 .clear = true,
                                 .depthTarget = depthTarget->get(),
                                 .clearDepth = 0.0f,
                                 .label = "lmx.test.depth.second"});
    secondFrame.bindPipeline(**pipeline);
    secondFrame.setUniforms(kVertexBufferSlot, first.data(), sizeof(first));
    secondFrame.draw(static_cast<uint32_t>(first.size()));
    secondFrame.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();
    (*target)->readback(pixels.data(), pixels.size());

    const Pixel occluded = pixelAt(pixels, 32, 40);
    INFO(describe("occluded", 32, 40, occluded));
    REQUIRE(occluded.r == 0);
    REQUIRE(occluded.g == 0);
    REQUIRE(occluded.b == 0);
    REQUIRE(occluded.a == 255);
}

//======================================================================================================================
// UVs span 0..2 so the right-side probes distinguish wrap from clamp without filtering ambiguity.
TEST_CASE("sampler address mode decides what a past-the-edge uv reads", "[gpu]") {
    using namespace lmx::rhi;

    constexpr uint32_t kSourceTextureSlot = 0;
    constexpr uint32_t kSamplerSlot = 0;

    constexpr std::array<Vertex, 12> kSplitQuads = {{
        {{-1.0f, -1.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
        {{-1.0f, -1.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
        {{-1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
        {{1.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
        {{1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
        {{1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
    }};

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto source = (*device)->createTexture({.width = 2,
                                            .height = 2,
                                            .format = Format::BGRA8Unorm,
                                            .renderTarget = true,
                                            .sampled = true,
                                            .label = "lmx.test.samplerSource"});
    INFO(errorOf(source));
    REQUIRE(source.has_value());

    const auto makeDestination = [&](const char* label) {
        return (*device)->createTexture({.width = kSize,
                                         .height = kSize,
                                         .format = Format::BGRA8Unorm,
                                         .renderTarget = true,
                                         .cpuReadback = true,
                                         .label = label});
    };
    auto wrapDestination = makeDestination("lmx.test.samplerWrapDestination");
    INFO(errorOf(wrapDestination));
    REQUIRE(wrapDestination.has_value());
    auto clampDestination = makeDestination("lmx.test.samplerClampDestination");
    INFO(errorOf(clampDestination));
    REQUIRE(clampDestination.has_value());

    auto fillLibrary = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(fillLibrary));
    REQUIRE(fillLibrary.has_value());

    auto fillPipeline =
        (*device)->createGraphicsPipeline({.library = fillLibrary->get(),
                                           .vertexEntry = "vertexMain",
                                           .fragmentEntry = "fragmentMain",
                                           .colorFormat = Format::BGRA8Unorm,
                                           .label = "lmx.test.samplerFillPipeline"});
    INFO(errorOf(fillPipeline));
    REQUIRE(fillPipeline.has_value());

    auto sampleLibrary = (*device)->loadShaderLibrary("Shaders/SamplerSmoke");
    INFO(errorOf(sampleLibrary));
    REQUIRE(sampleLibrary.has_value());

    auto samplePipeline =
        (*device)->createGraphicsPipeline({.library = sampleLibrary->get(),
                                           .vertexEntry = "vertexMain",
                                           .fragmentEntry = "fragmentMain",
                                           .colorFormat = Format::BGRA8Unorm,
                                           .label = "lmx.test.samplerSamplePipeline"});
    INFO(errorOf(samplePipeline));
    REQUIRE(samplePipeline.has_value());

    auto wrapSampler = (*device)->createSampler(
        {.addressMode = AddressMode::Wrap, .label = "lmx.test.wrapSampler"});
    INFO(errorOf(wrapSampler));
    REQUIRE(wrapSampler.has_value());

    auto clampSampler = (*device)->createSampler(
        {.addressMode = AddressMode::Clamp, .label = "lmx.test.clampSampler"});
    INFO(errorOf(clampSampler));
    REQUIRE(clampSampler.has_value());

    auto compareSampler = (*device)->createSampler(
        {.compare = CompareFunc::LessEqual, .label = "lmx.test.compareSampler"});
    INFO(errorOf(compareSampler));
    REQUIRE(compareSampler.has_value());

    CommandList& commands = (*device)->beginFrame();

    commands.beginRenderPass({.colorTarget = source->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.sampler.source"});
    commands.bindPipeline(**fillPipeline);
    commands.setUniforms(kVertexBufferSlot, kSplitQuads.data(), sizeof(kSplitQuads));
    commands.draw(static_cast<uint32_t>(kSplitQuads.size()));
    commands.endRenderPass();

    commands.textureBarrier(**source, TextureUse::RenderTarget, TextureUse::ShaderRead);

    const auto samplePass = [&](Texture& destination, Sampler& sampler) {
        commands.beginRenderPass({.colorTarget = &destination,
                                  .clearColor = {1.0f, 0.0f, 1.0f, 1.0f},
                                  .clear = true,
                                  .label = "lmx.test.sampler.probe"});
        commands.bindPipeline(**samplePipeline);
        commands.bindTexture(kSourceTextureSlot, **source);
        commands.bindSampler(kSamplerSlot, sampler);
        commands.draw(3);
        commands.endRenderPass();
    };
    samplePass(**wrapDestination, **wrapSampler);
    samplePass(**clampDestination, **clampSampler);

    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);

    const auto requireChannels = [&](const char* what, uint32_t x, float r, float g) {
        const Pixel probe = pixelAt(pixels, x, 32);
        INFO(describe(what, x, 32, probe));
        REQUIRE(channelIs(probe.r, r));
        REQUIRE(channelIs(probe.g, g));
        REQUIRE(probe.b == 0);
        REQUIRE(probe.a == 255);
    };

    (*wrapDestination)->readback(pixels.data(), pixels.size());
    requireChannels("wrap left", 8, 1.0f, 0.0f);
    requireChannels("wrap right", 24, 0.0f, 1.0f);
    requireChannels("wrap past the edge", 40, 1.0f, 0.0f);

    (*clampDestination)->readback(pixels.data(), pixels.size());
    requireChannels("clamp left", 8, 1.0f, 0.0f);
    requireChannels("clamp right", 24, 0.0f, 1.0f);
    requireChannels("clamp past the edge", 40, 0.0f, 1.0f);
}

namespace {} // namespace

//======================================================================================================================
// One uniform red BC1 block pins compressed upload stride and sampler-side block decoding.
TEST_CASE("a BC1 block decodes to its endpoint colour when sampled", "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint32_t kSourceTextureSlot = 0;

    constexpr std::array<uint8_t, 8> kRedBlock = {0x00, 0xF8, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00};
    const TextureMip mip{.data = kRedBlock.data(), .bytesPerRow = 8};

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto source = (*device)->createTexture({.width = 4,
                                            .height = 4,
                                            .format = Format::BC1Unorm,
                                            .sampled = true,
                                            .label = "lmx.test.bc1Source"},
                                           std::span{&mip, 1});
    INFO(errorOf(source));
    REQUIRE(source.has_value());

    auto destination = makeProbeTarget(**device, "lmx.test.bc1Destination");
    INFO(errorOf(destination));
    REQUIRE(destination.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/SamplerSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createGraphicsPipeline({.library = library->get(),
                                                       .vertexEntry = "vertexMain",
                                                       .fragmentEntry = "fragmentMain",
                                                       .colorFormat = Format::BGRA8Unorm,
                                                       .label = "lmx.test.bc1Pipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto sampler = (*device)->createSampler(
        {.addressMode = AddressMode::Clamp, .label = "lmx.test.bc1Sampler"});
    INFO(errorOf(sampler));
    REQUIRE(sampler.has_value());

    const std::vector<uint8_t> pixels = renderSampledImage(**device, **pipeline, kSourceTextureSlot,
                                                           **source, **sampler, **destination);

    const Pixel probe = pixelAt(pixels, 32, 32);
    INFO(describe("bc1 centre", 32, 32, probe));
    REQUIRE(channelIs(probe.r, 1.0f));
    REQUIRE(channelIs(probe.g, 0.0f));
    REQUIRE(channelIs(probe.b, 0.0f));
    REQUIRE(probe.a == 255);
}

//======================================================================================================================
// The same encoded byte in linear and sRGB textures must produce different values in a linear
// target.
TEST_CASE("an sRGB texture is linearised by the sampler, a linear one is not",
          "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint32_t kSourceTextureSlot = 0;
    constexpr uint8_t kEncoded = 188;
    constexpr int kDecoded = 128;

    constexpr std::array<uint8_t, 16> kGreyTexels = {
        kEncoded, kEncoded, kEncoded, 255, kEncoded, kEncoded, kEncoded, 255,
        kEncoded, kEncoded, kEncoded, 255, kEncoded, kEncoded, kEncoded, 255,
    };
    const TextureMip mip{.data = kGreyTexels.data(), .bytesPerRow = 2 * 4};

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    const auto makeSource = [&](Format format, const char* label) {
        return (*device)->createTexture(
            {.width = 2, .height = 2, .format = format, .sampled = true, .label = label},
            std::span{&mip, 1});
    };
    auto linearSource = makeSource(Format::RGBA8Unorm, "lmx.test.linearSource");
    INFO(errorOf(linearSource));
    REQUIRE(linearSource.has_value());
    auto srgbSource = makeSource(Format::RGBA8Unorm_sRGB, "lmx.test.srgbSource");
    INFO(errorOf(srgbSource));
    REQUIRE(srgbSource.has_value());

    auto linearDestination = makeProbeTarget(**device, "lmx.test.linearDestination");
    INFO(errorOf(linearDestination));
    REQUIRE(linearDestination.has_value());
    auto srgbDestination = makeProbeTarget(**device, "lmx.test.srgbDestination");
    INFO(errorOf(srgbDestination));
    REQUIRE(srgbDestination.has_value());
    auto viewDestination = makeProbeTarget(**device, "lmx.test.srgbViewDestination");
    INFO(errorOf(viewDestination));
    REQUIRE(viewDestination.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/SamplerSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createGraphicsPipeline({.library = library->get(),
                                                       .vertexEntry = "vertexMain",
                                                       .fragmentEntry = "fragmentMain",
                                                       .colorFormat = Format::BGRA8Unorm,
                                                       .label = "lmx.test.srgbPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto sampler = (*device)->createSampler(
        {.addressMode = AddressMode::Clamp, .label = "lmx.test.srgbSampler"});
    INFO(errorOf(sampler));
    REQUIRE(sampler.has_value());

    const std::vector<uint8_t> linearPixels = renderSampledImage(
        **device, **pipeline, kSourceTextureSlot, **linearSource, **sampler, **linearDestination);
    const Pixel linearProbe = pixelAt(linearPixels, 32, 32);
    INFO(describe("linear centre", 32, 32, linearProbe));
    REQUIRE(channelNear(linearProbe.r, kEncoded, 6));

    const std::vector<uint8_t> srgbPixels = renderSampledImage(
        **device, **pipeline, kSourceTextureSlot, **srgbSource, **sampler, **srgbDestination);
    const Pixel srgbProbe = pixelAt(srgbPixels, 32, 32);
    INFO(describe("srgb centre", 32, 32, srgbProbe));
    REQUIRE(channelNear(srgbProbe.r, kDecoded, 6));
    REQUIRE(channelNear(srgbProbe.g, kDecoded, 6));
    REQUIRE(channelNear(srgbProbe.b, kDecoded, 6));
    REQUIRE(srgbProbe.a == 255);

    // The allocation is linear, but this sampled view asks Metal to apply its sRGB sibling's
    // transfer function. It must match the native sRGB texture above without a second allocation.
    const std::vector<uint8_t> viewPixels =
        renderSampledImage(**device, **pipeline, kSourceTextureSlot, **linearSource, **sampler,
                           **viewDestination, {.format = Format::RGBA8Unorm_sRGB});
    const Pixel viewProbe = pixelAt(viewPixels, 32, 32);
    INFO(describe("sRGB view centre", 32, 32, viewProbe));
    REQUIRE(channelNear(viewProbe.r, kDecoded, 6));
    REQUIRE(channelNear(viewProbe.g, kDecoded, 6));
    REQUIRE(channelNear(viewProbe.b, kDecoded, 6));
    REQUIRE(viewProbe.a == 255);
}

//======================================================================================================================
// A uniform +X face isolates cubemap slice ordering and direction lookup.
TEST_CASE("a cubemap samples the face its direction points at", "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint32_t kCubeTextureSlot = 2;

    constexpr std::array<uint8_t, 4> kPlusX = {255, 0, 0, 255};      // red
    constexpr std::array<uint8_t, 4> kMinusX = {0, 255, 0, 255};     // green
    constexpr std::array<uint8_t, 4> kPlusY = {0, 0, 255, 255};      // blue
    constexpr std::array<uint8_t, 4> kMinusY = {0, 0, 255, 255};     // blue
    constexpr std::array<uint8_t, 4> kPlusZ = {255, 255, 255, 255};  // white
    constexpr std::array<uint8_t, 4> kMinusZ = {255, 255, 255, 255}; // white

    const std::array<TextureMip, 6> faces = {{
        {.data = kPlusX.data(), .bytesPerRow = 4},
        {.data = kMinusX.data(), .bytesPerRow = 4},
        {.data = kPlusY.data(), .bytesPerRow = 4},
        {.data = kMinusY.data(), .bytesPerRow = 4},
        {.data = kPlusZ.data(), .bytesPerRow = 4},
        {.data = kMinusZ.data(), .bytesPerRow = 4},
    }};

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto source = (*device)->createTexture({.width = 1,
                                            .height = 1,
                                            .format = Format::RGBA8Unorm,
                                            .kind = TextureKind::Cube,
                                            .sampled = true,
                                            .label = "lmx.test.cubeSource"},
                                           faces);
    INFO(errorOf(source));
    REQUIRE(source.has_value());

    auto destination = makeProbeTarget(**device, "lmx.test.cubeDestination");
    INFO(errorOf(destination));
    REQUIRE(destination.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/CubeSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createGraphicsPipeline({.library = library->get(),
                                                       .vertexEntry = "vertexMain",
                                                       .fragmentEntry = "fragmentMain",
                                                       .colorFormat = Format::BGRA8Unorm,
                                                       .label = "lmx.test.cubePipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto sampler = (*device)->createSampler(
        {.addressMode = AddressMode::Clamp, .label = "lmx.test.cubeSampler"});
    INFO(errorOf(sampler));
    REQUIRE(sampler.has_value());

    const std::vector<uint8_t> pixels = renderSampledImage(**device, **pipeline, kCubeTextureSlot,
                                                           **source, **sampler, **destination);

    const Pixel probe = pixelAt(pixels, 32, 32);
    INFO(describe("cube +X", 32, 32, probe));
    REQUIRE(channelIs(probe.r, 1.0f));
    REQUIRE(channelIs(probe.g, 0.0f));
    REQUIRE(channelIs(probe.b, 0.0f));
    REQUIRE(probe.a == 255);
}

//======================================================================================================================
// The interior and clear exterior pin depth storage, the pass barrier, and subsequent D32 sampling.
TEST_CASE("a depth-only pass stores depth a later pass can sample", "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint32_t kDepthTextureSlot = 0;
    constexpr uint32_t kSamplerSlot = 0;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto depthTarget = (*device)->createTexture({.width = kSize,
                                                 .height = kSize,
                                                 .format = Format::D32Float,
                                                 .renderTarget = true,
                                                 .sampled = true,
                                                 .label = "lmx.test.depthOnlyTarget"});
    INFO(errorOf(depthTarget));
    REQUIRE(depthTarget.has_value());

    auto destination = makeProbeTarget(**device, "lmx.test.depthOnlyDestination");
    INFO(errorOf(destination));
    REQUIRE(destination.has_value());

    auto triangleLibrary = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(triangleLibrary));
    REQUIRE(triangleLibrary.has_value());

    auto depthPipeline = (*device)->createGraphicsPipeline({.library = triangleLibrary->get(),
                                                            .vertexEntry = "vertexMain",
                                                            .fragmentEntry = "fragmentDepthOnly",
                                                            .colorFormat = Format::Unknown,
                                                            .depthFormat = Format::D32Float,
                                                            .depthTestEnable = true,
                                                            .depthWriteEnable = true,
                                                            .label = "lmx.test.depthOnlyPipeline"});
    INFO(errorOf(depthPipeline));
    REQUIRE(depthPipeline.has_value());

    auto sampleLibrary = (*device)->loadShaderLibrary("Shaders/SamplerSmoke");
    INFO(errorOf(sampleLibrary));
    REQUIRE(sampleLibrary.has_value());

    auto samplePipeline =
        (*device)->createGraphicsPipeline({.library = sampleLibrary->get(),
                                           .vertexEntry = "vertexMain",
                                           .fragmentEntry = "fragmentIdentityUv",
                                           .colorFormat = Format::BGRA8Unorm,
                                           .label = "lmx.test.depthSamplePipeline"});
    INFO(errorOf(samplePipeline));
    REQUIRE(samplePipeline.has_value());

    auto sampler = (*device)->createSampler(
        {.addressMode = AddressMode::Clamp, .label = "lmx.test.depthSampler"});
    INFO(errorOf(sampler));
    REQUIRE(sampler.has_value());

    CommandList& commands = (*device)->beginFrame();

    commands.beginRenderPass({.depthTarget = depthTarget->get(),
                              .clearDepth = 1.0f,
                              .storeDepth = true,
                              .label = "lmx.test.depthOnly.write"});
    commands.bindPipeline(**depthPipeline);
    commands.setUniforms(kVertexBufferSlot, kTriangle.data(), sizeof(kTriangle));
    commands.draw(static_cast<uint32_t>(kTriangle.size()));
    commands.endRenderPass();

    commands.textureBarrier(**depthTarget, TextureUse::RenderTarget, TextureUse::ShaderRead);

    commands.beginRenderPass({.colorTarget = destination->get(),
                              .clearColor = {1.0f, 0.0f, 1.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.depthOnly.probe"});
    commands.bindPipeline(**samplePipeline);
    commands.bindTexture(kDepthTextureSlot, **depthTarget);
    commands.bindSampler(kSamplerSlot, **sampler);
    commands.draw(3);
    commands.endRenderPass();

    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*destination)->readback(pixels.data(), pixels.size());

    const Pixel inside = pixelAt(pixels, 32, 32);
    INFO(describe("depth inside the triangle", 32, 32, inside));
    REQUIRE(inside.r < 8);

    const Pixel outside = pixelAt(pixels, 2, 2);
    INFO(describe("depth outside the triangle", 2, 2, outside));
    REQUIRE(outside.r == 255);
}

namespace {

constexpr std::array<Vertex, 3> kWhiteTriangle = {{
    {{0.0f, 0.5f}, {1.0f, 1.0f, 1.0f}},
    {{-0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}},
    {{0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}},
}};
constexpr std::array<Vertex, 3> kWhiteTriangleReversed = {{
    kWhiteTriangle[0],
    kWhiteTriangle[2],
    kWhiteTriangle[1],
}};

//======================================================================================================================
size_t coveredPixels(const std::vector<uint8_t>& bgra) {
    size_t covered = 0;
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const Pixel p = pixelAt(bgra, x, y);
            if (p.r > 128 || p.g > 128 || p.b > 128) {
                ++covered;
            }
        }
    }
    return covered;
}

} // namespace

//======================================================================================================================
// Coverage counts distinguish back-face culling, no culling, and wireframe fill without edge
// probes.
TEST_CASE("pipeline raster state culls back faces and draws wireframes", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    const auto makePipeline = [&](CullMode cull, FillMode fill, const char* label) {
        return (*device)->createGraphicsPipeline({.library = library->get(),
                                                  .vertexEntry = "vertexMain",
                                                  .fragmentEntry = "fragmentMain",
                                                  .colorFormat = Format::BGRA8Unorm,
                                                  .fillMode = fill,
                                                  .cullMode = cull,
                                                  .label = label});
    };
    auto cullBack = makePipeline(CullMode::Back, FillMode::Solid, "lmx.test.cullBackPipeline");
    INFO(errorOf(cullBack));
    REQUIRE(cullBack.has_value());
    auto cullNone = makePipeline(CullMode::None, FillMode::Solid, "lmx.test.cullNonePipeline");
    INFO(errorOf(cullNone));
    REQUIRE(cullNone.has_value());
    auto wireframe =
        makePipeline(CullMode::None, FillMode::Wireframe, "lmx.test.wireframePipeline");
    INFO(errorOf(wireframe));
    REQUIRE(wireframe.has_value());

    const auto makeDestination = [&](const char* label) {
        return (*device)->createTexture({.width = kSize,
                                         .height = kSize,
                                         .format = Format::BGRA8Unorm,
                                         .renderTarget = true,
                                         .cpuReadback = true,
                                         .label = label});
    };
    auto culled = makeDestination("lmx.test.culledDestination");
    INFO(errorOf(culled));
    REQUIRE(culled.has_value());
    auto kept = makeDestination("lmx.test.keptDestination");
    INFO(errorOf(kept));
    REQUIRE(kept.has_value());
    auto outlined = makeDestination("lmx.test.wireframeDestination");
    INFO(errorOf(outlined));
    REQUIRE(outlined.has_value());

    CommandList& commands = (*device)->beginFrame();
    const auto pass = [&](Texture& destination, GraphicsPipeline& pipeline,
                          const std::array<Vertex, 3>& vertices) {
        commands.beginRenderPass({.colorTarget = &destination,
                                  .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                                  .clear = true,
                                  .label = "lmx.test.rasterState"});
        commands.bindPipeline(pipeline);
        commands.setUniforms(kVertexBufferSlot, vertices.data(), sizeof(vertices));
        commands.draw(static_cast<uint32_t>(vertices.size()));
        commands.endRenderPass();
    };
    pass(**culled, **cullBack, kWhiteTriangleReversed);
    pass(**kept, **cullNone, kWhiteTriangleReversed);
    pass(**outlined, **wireframe, kWhiteTriangle);
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);

    (*culled)->readback(pixels.data(), pixels.size());
    INFO("clockwise triangle, CullMode::Back");
    REQUIRE(coveredPixels(pixels) == 0);

    (*kept)->readback(pixels.data(), pixels.size());
    INFO("clockwise triangle, CullMode::None");
    const size_t solidCoverage = coveredPixels(pixels);
    INFO("solid coverage: " + std::to_string(solidCoverage));
    REQUIRE(solidCoverage > 450);
    REQUIRE(solidCoverage < 570);

    (*outlined)->readback(pixels.data(), pixels.size());
    const Pixel interior = pixelAt(pixels, 32, 40);
    INFO(describe("wireframe interior", 32, 40, interior));
    REQUIRE(interior.r == 0);
    REQUIRE(interior.g == 0);
    REQUIRE(interior.b == 0);
    const size_t wireCoverage = coveredPixels(pixels);
    INFO("wireframe coverage: " + std::to_string(wireCoverage));
    REQUIRE(wireCoverage > 40);
    REQUIRE(wireCoverage < 250);
}

//======================================================================================================================
// A GPU timestamp is readable only once the frame that wrote it retired, so nothing is reportable
// until a beginFrame observes that retirement -- not at device creation, and not even after the
// measured frame's own waitIdle.
TEST_CASE("pass timings stay empty until a frame retirement is observed", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    REQUIRE((*device)->passTimings().empty());

    auto target = makeProbeTarget(**device, "lmx.test.timingIdleTarget");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.timing.idle"});
    commands.endRenderPass();
    REQUIRE((*device)->passTimings().empty());
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    REQUIRE((*device)->passTimings().empty());
}

//======================================================================================================================
TEST_CASE("pass timings report every pass of the retired frame", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createGraphicsPipeline({.library = library->get(),
                                                       .vertexEntry = "vertexMain",
                                                       .fragmentEntry = "fragmentMain",
                                                       .colorFormat = Format::BGRA8Unorm,
                                                       .label = "lmx.test.timingPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto first = makeProbeTarget(**device, "lmx.test.timingFirstTarget");
    INFO(errorOf(first));
    REQUIRE(first.has_value());
    auto second = makeProbeTarget(**device, "lmx.test.timingSecondTarget");
    INFO(errorOf(second));
    REQUIRE(second.has_value());

    const auto frameStart = std::chrono::steady_clock::now();
    CommandList& commands = (*device)->beginFrame();
    const auto pass = [&](Texture& destination, const char* label) {
        commands.beginRenderPass({.colorTarget = &destination,
                                  .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                                  .clear = true,
                                  .label = label});
        commands.bindPipeline(**pipeline);
        commands.setUniforms(kVertexBufferSlot, kTriangle.data(), sizeof(kTriangle));
        commands.draw(static_cast<uint32_t>(kTriangle.size()));
        commands.endRenderPass();
    };
    pass(**first, "lmx.test.timing.first");
    pass(**second, "lmx.test.timing.second");
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();
    // Every tick these passes are credited with was spent inside this window: the work was not
    // submitted before beginFrame and had finished before waitIdle returned. It is the one bound
    // available without a second clock, and it is what a wrong tick-to-millisecond scale breaks --
    // a raw tick delta is positive whatever the divisor is.
    const std::chrono::duration<double, std::milli> wall =
        std::chrono::steady_clock::now() - frameStart;

    // waitIdle retires the measured frame; the next beginFrame is what publishes its counters.
    (*device)->beginFrame();
    (*device)->endFrame(nullptr);

    const std::span<const PassTiming> timings = (*device)->passTimings();
    REQUIRE(timings.size() == 2);
    REQUIRE(timings[0].label == "lmx.test.timing.first");
    REQUIRE(timings[1].label == "lmx.test.timing.second");
    double reported = 0.0;
    for (const PassTiming& timing : timings) {
        INFO(timing.label + ": " + std::to_string(timing.gpuMilliseconds) + " ms of " +
             std::to_string(wall.count()) + " ms wall");
        REQUIRE(timing.gpuMilliseconds > 0.0);
        REQUIRE(timing.gpuMilliseconds < wall.count());
        reported += timing.gpuMilliseconds;
    }
    INFO("reported " + std::to_string(reported) + " ms across " + std::to_string(wall.count()) +
         " ms wall");
    REQUIRE(reported < wall.count());
}

namespace {

//======================================================================================================================
// Bit pattern of `value` in binary16 -- the layout an RGBA16Float readback hands back. The
// round-trip check pins every caller to a value binary16 holds exactly, which is what lets the
// oracle below compare readback bits for equality rather than within a tolerance.
uint16_t halfBits(float value) {
    const _Float16 half = static_cast<_Float16>(value);
    REQUIRE(static_cast<float>(half) == value);
    uint16_t bits = 0;
    std::memcpy(&bits, &half, sizeof(bits));
    return bits;
}

// One RGBA16Float texel, in the channel order readback() produces.
struct HalfPixel {
    uint16_t r = 0, g = 0, b = 0, a = 0;
};

//======================================================================================================================
HalfPixel halfPixelAt(const std::vector<uint16_t>& rgba, uint32_t x, uint32_t y) {
    const size_t offset = (size_t{y} * kSize + x) * 4;
    return {rgba[offset], rgba[offset + 1], rgba[offset + 2], rgba[offset + 3]};
}

//======================================================================================================================
std::string describeHalf(const char* what, uint32_t x, uint32_t y, const HalfPixel& p) {
    return std::string(what) + " (" + std::to_string(x) + "," + std::to_string(y) +
           "): R=" + std::to_string(p.r) + " G=" + std::to_string(p.g) +
           " B=" + std::to_string(p.b) + " A=" + std::to_string(p.a) + " (binary16 bits)";
}

} // namespace

//======================================================================================================================
// An 8-bit target clamps at 1.0, which is exactly what a scene-linear target must not do: the
// values above it are the radiance later stages tone map. Both the hardware clear and the fragment
// write are probed, since they reach the attachment by different paths, and every constant is
// exactly representable in binary16 -- 1.5, 2.0, 4.0, 8.0, 0.5, -0.25 -- so the assertions are
// equalities rather than tolerances.
TEST_CASE("an RGBA16Float target keeps values above 1.0 through readback", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto target = (*device)->createTexture({.width = kSize,
                                            .height = kSize,
                                            .format = Format::RGBA16Float,
                                            .renderTarget = true,
                                            .cpuReadback = true,
                                            .label = "lmx.test.hdrTarget"});
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createGraphicsPipeline({.library = library->get(),
                                                       .vertexEntry = "vertexMain",
                                                       .fragmentEntry = "fragmentMain",
                                                       .colorFormat = Format::RGBA16Float,
                                                       .label = "lmx.test.hdrPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    // A flat colour across all three vertices: interpolating equal values is exact, so the
    // fragment writes the authored number itself and the readback can be compared bit for bit.
    std::array<Vertex, 3> triangle = kTriangle;
    for (Vertex& vertex : triangle) {
        vertex.color[0] = 1.5f;
        vertex.color[1] = 2.0f;
        vertex.color[2] = 4.0f;
    }

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.5f, -0.25f, 8.0f, 0.25f},
                              .clear = true,
                              .label = "lmx.test.hdr.write"});
    commands.bindPipeline(**pipeline);
    commands.setUniforms(kVertexBufferSlot, triangle.data(), sizeof(triangle));
    commands.draw(static_cast<uint32_t>(triangle.size()));
    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint16_t> texels(size_t{kSize} * kSize * 4);
    (*target)->readback(texels.data(), texels.size() * sizeof(uint16_t));

    const HalfPixel inside = halfPixelAt(texels, 32, 40);
    INFO(describeHalf("inside the triangle", 32, 40, inside));
    REQUIRE(inside.r == halfBits(1.5f));
    REQUIRE(inside.g == halfBits(2.0f));
    REQUIRE(inside.b == halfBits(4.0f));
    REQUIRE(inside.a == halfBits(1.0f));

    const HalfPixel cleared = halfPixelAt(texels, 2, 2);
    INFO(describeHalf("cleared", 2, 2, cleared));
    REQUIRE(cleared.r == halfBits(0.5f));
    REQUIRE(cleared.g == halfBits(-0.25f));
    REQUIRE(cleared.b == halfBits(8.0f));
    REQUIRE(cleared.a == halfBits(0.25f));
}

namespace {

constexpr uint32_t kDepthPassSlot = 2;

// Mirrors ShadowSmoke.slang's PassUniforms. That shader's vertex entry is the one in this suite
// whose vertices carry a z of their own, which is what a depth-compare oracle needs; identity
// transforms pass the z straight through to clip space at w = 1, so a vertex's z is the depth
// Metal compares.
struct DepthPassUniforms {
    glm::mat4 lightViewProj{1.0f};
    glm::mat4 shadowTransform{1.0f};
    int32_t filter = 0;
    int32_t pad[3] = {0, 0, 0};
};
static_assert(sizeof(DepthPassUniforms) == 144, "must match ShadowSmoke.slang's PassUniforms");

struct DepthVertex {
    float x = 0.f, y = 0.f, z = 0.f;
};
static_assert(sizeof(DepthVertex) == 12, "must match Slang's packed_float3 Vertex layout");

//======================================================================================================================
// Counter-clockwise seen from the front, matching the project winding, so the default back-face
// culling keeps it. halfExtent 1 covers the whole target, which is what a case needs when every
// texel of the result has to hold the same answer.
std::array<DepthVertex, 6> depthQuad(float z, float halfExtent = 0.5f) {
    const float lo = -halfExtent;
    const float hi = halfExtent;
    return {{
        {lo, lo, z},
        {hi, lo, z},
        {hi, hi, z},
        {lo, lo, z},
        {hi, hi, z},
        {lo, hi, z},
    }};
}

} // namespace

//======================================================================================================================
// Reversed-Z puts the near plane at 1 and the far plane at 0, so "nearer" is the numerically larger
// depth and the winning comparison is Greater against a clear of 0. The three draws below cover
// both directions of that rule in one sequence: 0.25 beats the clear, 0.75 replaces it because it
// is nearer, and 0.5 is then rejected because it is not. Under the Less semantics this replaces,
// every draw would fail against the 0.0 clear and the probe would read the clear back instead.
TEST_CASE("a Greater depth test keeps the nearer fragment in reversed-Z", "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint32_t kDepthTextureSlot = 0;
    constexpr uint32_t kSamplerSlot = 0;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto depthTarget = (*device)->createTexture({.width = kSize,
                                                 .height = kSize,
                                                 .format = Format::D32Float,
                                                 .renderTarget = true,
                                                 .sampled = true,
                                                 .label = "lmx.test.reversedDepthTarget"});
    INFO(errorOf(depthTarget));
    REQUIRE(depthTarget.has_value());

    auto destination = makeProbeTarget(**device, "lmx.test.reversedDepthDestination");
    INFO(errorOf(destination));
    REQUIRE(destination.has_value());

    auto depthLibrary = (*device)->loadShaderLibrary("Shaders/ShadowSmoke");
    INFO(errorOf(depthLibrary));
    REQUIRE(depthLibrary.has_value());

    auto depthPipeline =
        (*device)->createGraphicsPipeline({.library = depthLibrary->get(),
                                           .vertexEntry = "vertexMain",
                                           .fragmentEntry = "fragmentDepthOnly",
                                           .colorFormat = Format::Unknown,
                                           .depthFormat = Format::D32Float,
                                           .depthTestEnable = true,
                                           .depthWriteEnable = true,
                                           .depthCompare = DepthCompare::Greater,
                                           .label = "lmx.test.reversedDepthPipeline"});
    INFO(errorOf(depthPipeline));
    REQUIRE(depthPipeline.has_value());

    auto sampleLibrary = (*device)->loadShaderLibrary("Shaders/SamplerSmoke");
    INFO(errorOf(sampleLibrary));
    REQUIRE(sampleLibrary.has_value());

    auto samplePipeline =
        (*device)->createGraphicsPipeline({.library = sampleLibrary->get(),
                                           .vertexEntry = "vertexMain",
                                           .fragmentEntry = "fragmentIdentityUv",
                                           .colorFormat = Format::BGRA8Unorm,
                                           .label = "lmx.test.reversedDepthSamplePipeline"});
    INFO(errorOf(samplePipeline));
    REQUIRE(samplePipeline.has_value());

    auto sampler = (*device)->createSampler(
        {.addressMode = AddressMode::Clamp, .label = "lmx.test.reversedDepthSampler"});
    INFO(errorOf(sampler));
    REQUIRE(sampler.has_value());

    const DepthPassUniforms uniforms{};
    const std::array<DepthVertex, 6> far = depthQuad(0.25f);
    const std::array<DepthVertex, 6> near = depthQuad(0.75f);
    const std::array<DepthVertex, 6> middle = depthQuad(0.5f);

    CommandList& commands = (*device)->beginFrame();

    commands.beginRenderPass({.depthTarget = depthTarget->get(),
                              .clearDepth = 0.0f,
                              .storeDepth = true,
                              .label = "lmx.test.reversedDepth.write"});
    commands.bindPipeline(**depthPipeline);
    commands.setUniforms(kDepthPassSlot, &uniforms, sizeof(uniforms));
    const auto drawQuad = [&](const std::array<DepthVertex, 6>& quad) {
        commands.setUniforms(kVertexBufferSlot, quad.data(), sizeof(quad));
        commands.draw(static_cast<uint32_t>(quad.size()));
    };
    drawQuad(far);
    drawQuad(near);
    drawQuad(middle);
    commands.endRenderPass();

    commands.textureBarrier(**depthTarget, TextureUse::RenderTarget, TextureUse::ShaderRead);

    commands.beginRenderPass({.colorTarget = destination->get(),
                              .clearColor = {1.0f, 0.0f, 1.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.reversedDepth.probe"});
    commands.bindPipeline(**samplePipeline);
    commands.bindTexture(kDepthTextureSlot, **depthTarget);
    commands.bindSampler(kSamplerSlot, **sampler);
    commands.draw(3);
    commands.endRenderPass();

    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*destination)->readback(pixels.data(), pixels.size());

    // 0.75 through an 8-bit probe is 191.25; the tolerance covers the quantization and nothing
    // else -- the losing depths would read 64 (0.25), 128 (0.5) and 0 (the clear).
    const Pixel kept = pixelAt(pixels, 32, 32);
    INFO(describe("inside the quads", 32, 32, kept));
    REQUIRE(channelNear(kept.r, 191, 2));

    const Pixel untouched = pixelAt(pixels, 2, 2);
    INFO(describe("outside the quads", 2, 2, untouched));
    REQUIRE(untouched.r == 0);
}

namespace {

constexpr uint32_t kCompareTextureSlot = 3;
constexpr uint32_t kCompareSamplerSlot = 1;

//======================================================================================================================
// The NDC-to-texcoord map fitShadowOrtho bakes into shadowTransform: x [-1,1] -> u [0,1] and
// y [-1,1] -> v [1,0]. Written out here rather than borrowed because this suite tests the RHI,
// which knows nothing of Render's matrices -- what it shares with them is only the convention.
glm::mat4 ndcToTexcoord() {
    glm::mat4 map{1.0f};
    map[0][0] = 0.5f;
    map[1][1] = -0.5f;
    map[3][0] = 0.5f;
    map[3][1] = 0.5f;
    return map;
}

} // namespace

//======================================================================================================================
// A comparison sampler answers a question, and its CompareFunc is the question. Everything else
// about a shadow lookup can be right while that one field is wrong, and the result is a scene lit
// exactly where it should be dark -- so it gets an oracle that reads the two functions back to
// back off one depth map and one receiver.
//
// The map holds 0.25 everywhere and the receiver draws at 0.75, which under reversed-Z means the
// receiver is *nearer* the light than anything recorded and must come out fully lit. PCF adds its
// 0.004 bias to the reference, so the sampler is asked to compare 0.754 against 0.25:
// GreaterEqual answers 1 and the probe reads white, LessEqual answers 0 and it reads black. The
// two are each other's complement here, which is what makes a swapped CompareFunc impossible to
// mistake for a filtering or addressing problem.
TEST_CASE("a comparison sampler's function decides which depth reads as lit", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto depthTarget = (*device)->createTexture({.width = kSize,
                                                 .height = kSize,
                                                 .format = Format::D32Float,
                                                 .renderTarget = true,
                                                 .sampled = true,
                                                 .label = "lmx.test.compareDepthTarget"});
    INFO(errorOf(depthTarget));
    REQUIRE(depthTarget.has_value());

    auto greaterEqualImage = makeProbeTarget(**device, "lmx.test.compareGreaterEqualImage");
    INFO(errorOf(greaterEqualImage));
    REQUIRE(greaterEqualImage.has_value());
    auto lessEqualImage = makeProbeTarget(**device, "lmx.test.compareLessEqualImage");
    INFO(errorOf(lessEqualImage));
    REQUIRE(lessEqualImage.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/ShadowSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto depthPipeline =
        (*device)->createGraphicsPipeline({.library = library->get(),
                                           .vertexEntry = "vertexMain",
                                           .fragmentEntry = "fragmentDepthOnly",
                                           .colorFormat = Format::Unknown,
                                           .depthFormat = Format::D32Float,
                                           .depthTestEnable = true,
                                           .depthWriteEnable = true,
                                           .depthCompare = DepthCompare::Greater,
                                           .label = "lmx.test.compareDepthPipeline"});
    INFO(errorOf(depthPipeline));
    REQUIRE(depthPipeline.has_value());

    auto comparePipeline = (*device)->createGraphicsPipeline({.library = library->get(),
                                                              .vertexEntry = "vertexMain",
                                                              .fragmentEntry = "fragmentMain",
                                                              .colorFormat = Format::BGRA8Unorm,
                                                              .label = "lmx.test.comparePipeline"});
    INFO(errorOf(comparePipeline));
    REQUIRE(comparePipeline.has_value());

    const auto makeCompareSampler = [&](CompareFunc compare, const char* label) {
        return (*device)->createSampler({.filter = FilterMode::Linear,
                                         .addressMode = AddressMode::Clamp,
                                         .compare = compare,
                                         .label = label});
    };
    auto greaterEqualSampler =
        makeCompareSampler(CompareFunc::GreaterEqual, "lmx.test.greaterEqualSampler");
    INFO(errorOf(greaterEqualSampler));
    REQUIRE(greaterEqualSampler.has_value());
    auto lessEqualSampler = makeCompareSampler(CompareFunc::LessEqual, "lmx.test.lessEqualSampler");
    INFO(errorOf(lessEqualSampler));
    REQUIRE(lessEqualSampler.has_value());

    DepthPassUniforms uniforms{};
    uniforms.shadowTransform = ndcToTexcoord();

    // Both quads cover the whole target, so the map is uniformly 0.25 and PCF's kernel reads the
    // same texel value at every offset -- there is no edge for a filtering difference to hide in.
    const std::array<DepthVertex, 6> blocker = depthQuad(0.25f, 1.0f);
    const std::array<DepthVertex, 6> receiver = depthQuad(0.75f, 1.0f);

    CommandList& commands = (*device)->beginFrame();

    commands.beginRenderPass({.depthTarget = depthTarget->get(),
                              .clearDepth = 0.0f,
                              .storeDepth = true,
                              .label = "lmx.test.compare.write"});
    commands.bindPipeline(**depthPipeline);
    commands.setUniforms(kDepthPassSlot, &uniforms, sizeof(uniforms));
    commands.setUniforms(kVertexBufferSlot, blocker.data(), sizeof(blocker));
    commands.draw(static_cast<uint32_t>(blocker.size()));
    commands.endRenderPass();

    commands.textureBarrier(**depthTarget, TextureUse::RenderTarget, TextureUse::ShaderRead);

    const auto comparePass = [&](Texture& destination, Sampler& sampler) {
        commands.beginRenderPass({.colorTarget = &destination,
                                  .clearColor = {1.0f, 0.0f, 1.0f, 1.0f},
                                  .clear = true,
                                  .label = "lmx.test.compare.probe"});
        commands.bindPipeline(**comparePipeline);
        commands.bindTexture(kCompareTextureSlot, **depthTarget);
        commands.bindSampler(kCompareSamplerSlot, sampler);
        commands.setUniforms(kDepthPassSlot, &uniforms, sizeof(uniforms));
        commands.setUniforms(kVertexBufferSlot, receiver.data(), sizeof(receiver));
        commands.draw(static_cast<uint32_t>(receiver.size()));
        commands.endRenderPass();
    };
    comparePass(**greaterEqualImage, **greaterEqualSampler);
    comparePass(**lessEqualImage, **lessEqualSampler);

    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);

    // Two probes rather than one: a lookup that answered correctly only where the kernel happened
    // to stay inside the texture would read differently at the centre and near the corner.
    const auto requireUniform = [&](const char* what, int want) {
        for (const auto& at :
             {std::pair<uint32_t, uint32_t>{32, 32}, std::pair<uint32_t, uint32_t>{4, 4}}) {
            const Pixel probe = pixelAt(pixels, at.first, at.second);
            INFO(describe(what, at.first, at.second, probe));
            REQUIRE(int{probe.r} == want);
            REQUIRE(int{probe.g} == want);
            REQUIRE(int{probe.b} == want);
        }
    };

    (*greaterEqualImage)->readback(pixels.data(), pixels.size());
    requireUniform("GreaterEqual: receiver nearer than the blocker", 255);

    (*lessEqualImage)->readback(pixels.data(), pixels.size());
    requireUniform("LessEqual: the same geometry, the opposite answer", 0);
}
