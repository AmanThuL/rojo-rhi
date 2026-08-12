#include "GpuTestSupport.h"

namespace {

// Mirrors BufferHazardSmoke.slang's HazardParams; the layout is pinned by the shader.
struct HazardParams {
    uint32_t bias = 0;
};

// Mirrors ComputeImageSmoke.slang's ImageParams.
struct CopyImageParams {
    uint32_t extent = 0;
};

// The argument-table slots BufferHazardSmoke.slang's globals compile to.
constexpr uint32_t kHazardOutputSlot = 0;
constexpr uint32_t kHazardSourceSlot = 1;
constexpr uint32_t kHazardParamsSlot = 2;

// computeAddFromBuffer's [numthreads], and the image kernels' in x and y.
constexpr uint32_t kHazardThreadsPerGroup = 64;
constexpr uint32_t kCopyImageThreadsPerGroup = 8;

// ComputeImageSmoke.slang's slots, repeated here rather than shared with GpuComputeTests.cpp:
// each test file states the binding contract of the shader it drives.
constexpr uint32_t kCopyImageSlot = 0;
constexpr uint32_t kCopyImageParamsSlot = 1;

// One BGRA8Unorm texel of a cubemap face, as the upload writes it and the readback reads it. The
// face and level are both encoded so a copy that silently resolved to face zero or level zero
// disagrees on a channel rather than on nothing.
//======================================================================================================================
std::array<uint8_t, 4> faceTexel(uint32_t face, uint32_t level) {
    return {static_cast<uint8_t>(16 + face * 20), static_cast<uint8_t>(80 + level * 40), 0xAB,
            0xFF};
}

//======================================================================================================================
// The gradient computeWriteImage writes, at a texel of a square image of the given extent.
uint8_t copyGradientChannel(uint32_t coordinate, uint32_t extent) {
    const float value = static_cast<float>(coordinate) / static_cast<float>(extent - 1);
    return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

} // namespace

//======================================================================================================================
// The destination range is written at a non-zero offset and the bytes on either side of it are
// asserted untouched, so a copy that ignored either offset fails on a specific byte rather than on
// a whole-buffer comparison that could pass by accident.
TEST_CASE("a copy pass copies a byte range between two buffers", "[gpu]") {
    using namespace lmx::rhi;

    constexpr uint64_t kBufferBytes = 256;
    constexpr uint64_t kSourceOffset = 64;
    constexpr uint64_t kDestinationOffset = 128;
    constexpr uint64_t kCopyBytes = 64;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    std::vector<uint8_t> sourceBytes(kBufferBytes);
    for (size_t index = 0; index < sourceBytes.size(); ++index) {
        sourceBytes[index] = static_cast<uint8_t>(index);
    }
    std::vector<uint8_t> destinationBytes(kBufferBytes, 0xEE);

    auto source = (*device)->createBuffer({.size = kBufferBytes, .label = "lmx.test.copy.source"},
                                          sourceBytes.data());
    INFO(errorOf(source));
    REQUIRE(source.has_value());

    auto destination = (*device)->createBuffer(
        {.size = kBufferBytes, .cpuReadback = true, .label = "lmx.test.copy.destination"},
        destinationBytes.data());
    INFO(errorOf(destination));
    REQUIRE(destination.has_value());

    CommandList& commands = (*device)->beginFrame();
    commands.beginCopyPass("lmx.test.copy.buffers");
    commands.copyBuffer(**source, kSourceOffset, **destination, kDestinationOffset, kCopyBytes);
    commands.endCopyPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> result(kBufferBytes, 0);
    (*destination)->readback(result.data(), result.size());

    for (uint64_t index = 0; index < kBufferBytes; ++index) {
        INFO("byte " + std::to_string(index));
        const bool copied = index >= kDestinationOffset && index < kDestinationOffset + kCopyBytes;
        const uint8_t expected =
            copied ? sourceBytes[index - kDestinationOffset + kSourceOffset] : 0xEE;
        REQUIRE(result[index] == expected);
    }
}

//======================================================================================================================
// fillBuffer covers a middle range only, so the case pins both the value and the extent of what it
// touches: a fill that ran over the whole allocation would pass a check of the filled bytes alone.
TEST_CASE("a copy pass fills a buffer range with a byte value", "[gpu]") {
    using namespace lmx::rhi;

    constexpr uint64_t kBufferBytes = 256;
    constexpr uint64_t kFillOffset = 64;
    constexpr uint64_t kFillBytes = 128;
    constexpr uint8_t kFillValue = 0x5A;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    const std::vector<uint8_t> initial(kBufferBytes, 0xEE);
    auto buffer = (*device)->createBuffer(
        {.size = kBufferBytes, .cpuReadback = true, .label = "lmx.test.copy.filled"},
        initial.data());
    INFO(errorOf(buffer));
    REQUIRE(buffer.has_value());

    CommandList& commands = (*device)->beginFrame();
    commands.beginCopyPass("lmx.test.copy.fill");
    commands.fillBuffer(**buffer, kFillOffset, kFillBytes, kFillValue);
    commands.endCopyPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> result(kBufferBytes, 0);
    (*buffer)->readback(result.data(), result.size());

    for (uint64_t index = 0; index < kBufferBytes; ++index) {
        INFO("byte " + std::to_string(index));
        const bool filled = index >= kFillOffset && index < kFillOffset + kFillBytes;
        REQUIRE(result[index] == (filled ? kFillValue : 0xEE));
    }
}

//======================================================================================================================
// The buffer hazard the barrier exists for, in the shape the histogram feature needs: a copy pass
// clears an accumulation buffer, a barrier orders it, and a dispatch reads what the clear wrote.
// The buffer starts at a sentinel the fill overwrites, so a dispatch that ran before the fill --
// or read stale bytes -- reports the sentinel plus the bias instead.
TEST_CASE("a filled buffer is read by a later dispatch through a buffer barrier", "[gpu]") {
    using namespace lmx::rhi;

    constexpr uint32_t kElements = 256;
    constexpr uint32_t kBias = 7;
    // Every byte of the fill is this value, so each uint32 element reads back as 0x03030303.
    constexpr uint8_t kFillValue = 0x03;
    constexpr uint32_t kFilledElement = 0x03030303;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/BufferHazardSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline =
        (*device)->createComputePipeline({.library = library->get(),
                                          .computeEntry = "computeAddFromBuffer",
                                          .threadsPerThreadgroup = {kHazardThreadsPerGroup, 1, 1},
                                          .label = "lmx.test.copy.hazardPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    const std::vector<uint32_t> sentinel(kElements, 0xFFFFFFFFu);
    auto accumulator = (*device)->createBuffer({.size = sizeof(uint32_t) * kElements,
                                                .storageRead = true,
                                                .label = "lmx.test.copy.hazardAccumulator"},
                                               sentinel.data());
    INFO(errorOf(accumulator));
    REQUIRE(accumulator.has_value());

    auto resolved = (*device)->createBuffer({.size = sizeof(uint32_t) * kElements,
                                             .storageWrite = true,
                                             .cpuReadback = true,
                                             .label = "lmx.test.copy.hazardResolved"},
                                            nullptr);
    INFO(errorOf(resolved));
    REQUIRE(resolved.has_value());

    const HazardParams params{.bias = kBias};

    CommandList& commands = (*device)->beginFrame();
    commands.beginCopyPass("lmx.test.copy.hazardClear");
    commands.fillBuffer(**accumulator, 0, sizeof(uint32_t) * kElements, kFillValue);
    commands.endCopyPass();

    commands.bufferBarrier(**accumulator, BufferUse::CopyDestination, BufferUse::StorageRead);

    commands.beginComputePass("lmx.test.copy.hazardAccumulate");
    commands.bindComputePipeline(**pipeline);
    commands.bindStorageBuffer(kHazardOutputSlot, **resolved, StorageAccess::Write);
    commands.bindStorageBuffer(kHazardSourceSlot, **accumulator, StorageAccess::Read);
    commands.bindFrameData(kHazardParamsSlot, params);
    commands.dispatch(kElements / kHazardThreadsPerGroup, 1, 1);
    commands.endComputePass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint32_t> values(kElements, 0);
    (*resolved)->readback(values.data(), values.size() * sizeof(uint32_t));

    for (uint32_t index = 0; index < kElements; ++index) {
        INFO("element " + std::to_string(index));
        REQUIRE(values[index] == kFilledElement + kBias);
    }
}

//======================================================================================================================
// Texture::readback covers only the whole of level zero, so a mip of a GPU-produced chain is
// reachable only through a copy. Level 1 carries its own gradient, which differs from level 0's at
// every interior texel -- a copy that silently resolved to level 0 reads back the wrong values
// rather than nothing.
TEST_CASE("a copy captures an intermediate mip level of a GPU-written chain",
          "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint32_t kMipExtent = kSize / 2;
    constexpr uint32_t kMipBytes = kMipExtent * kMipExtent * 4;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/ComputeImageSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createComputePipeline(
        {.library = library->get(),
         .computeEntry = "computeWriteImage",
         .threadsPerThreadgroup = {kCopyImageThreadsPerGroup, kCopyImageThreadsPerGroup, 1},
         .label = "lmx.test.copy.mipWritePipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto chain = (*device)->createTexture({.width = kSize,
                                           .height = kSize,
                                           .format = Format::RGBA8Unorm,
                                           .mipLevels = 2,
                                           .storageWrite = true,
                                           .label = "lmx.test.copy.mipChain"});
    INFO(errorOf(chain));
    REQUIRE(chain.has_value());

    auto staging = (*device)->createBuffer(
        {.size = kMipBytes, .cpuReadback = true, .label = "lmx.test.copy.mipStaging"}, nullptr);
    INFO(errorOf(staging));
    REQUIRE(staging.has_value());

    const CopyImageParams params{.extent = kMipExtent};
    const TextureViewDesc level1{.range = {.baseMipLevel = 1, .mipLevelCount = 1}};

    CommandList& commands = (*device)->beginFrame();
    commands.beginComputePass("lmx.test.copy.mipWrite");
    commands.bindComputePipeline(**pipeline);
    commands.bindStorageTexture(kCopyImageSlot, **chain, level1, StorageAccess::Write);
    commands.bindFrameData(kCopyImageParamsSlot, params);
    commands.dispatch(kMipExtent / kCopyImageThreadsPerGroup,
                      kMipExtent / kCopyImageThreadsPerGroup, 1);
    commands.endComputePass();

    commands.textureBarrier(**chain, {.baseMipLevel = 1, .mipLevelCount = 1},
                            TextureUse::StorageWrite, TextureUse::CopySource);

    commands.beginCopyPass("lmx.test.copy.mipCapture");
    commands.copyTextureToBuffer(**chain,
                                 {.mipLevel = 1, .width = kMipExtent, .height = kMipExtent},
                                 **staging, {.bytesPerRow = kMipExtent * 4});
    commands.endCopyPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(kMipBytes, 0);
    (*staging)->readback(pixels.data(), pixels.size());

    const std::array<std::pair<uint32_t, uint32_t>, 4> probes = {
        {{0, 0}, {kMipExtent - 1, 0}, {0, kMipExtent - 1}, {11, 20}}};
    for (const auto& [x, y] : probes) {
        const size_t offset = (size_t{y} * kMipExtent + x) * 4;
        INFO("mip 1 texel (" + std::to_string(x) + "," + std::to_string(y) + ")");
        REQUIRE(channelNear(pixels[offset], copyGradientChannel(x, kMipExtent), 1));
        REQUIRE(channelNear(pixels[offset + 1], copyGradientChannel(y, kMipExtent), 1));
        REQUIRE(channelNear(pixels[offset + 2], 64, 1));
        REQUIRE(pixels[offset + 3] == 255);
    }
}

//======================================================================================================================
// The array-layer half of the same contract, on the only multi-layer texture kind this RHI has.
// Every face and level carries a distinct constant, so a copy that resolved to the wrong slice or
// the wrong level disagrees on a channel.
TEST_CASE("a copy captures one array layer and mip of a cubemap", "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint32_t kFaceExtent = 16;
    constexpr uint32_t kLevels = 2;
    constexpr uint32_t kFaces = 6;
    constexpr uint32_t kCapturedFace = 4;
    constexpr uint32_t kCapturedLevel = 1;
    constexpr uint32_t kCapturedExtent = kFaceExtent >> kCapturedLevel;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    // Mip-major per face, exactly as createTexture documents the span.
    std::array<std::vector<uint8_t>, kFaces * kLevels> faceData;
    std::array<TextureMip, kFaces * kLevels> mips{};
    for (uint32_t face = 0; face < kFaces; ++face) {
        for (uint32_t level = 0; level < kLevels; ++level) {
            const uint32_t extent = kFaceExtent >> level;
            const std::array<uint8_t, 4> texel = faceTexel(face, level);
            const size_t index = size_t{face} * kLevels + level;
            faceData[index].resize(size_t{extent} * extent * 4);
            for (size_t byte = 0; byte < faceData[index].size(); ++byte) {
                faceData[index][byte] = texel[byte % 4];
            }
            mips[index] = {.data = faceData[index].data(), .bytesPerRow = uint64_t{extent} * 4};
        }
    }

    auto cube = (*device)->createTexture({.width = kFaceExtent,
                                          .height = kFaceExtent,
                                          .format = Format::BGRA8Unorm,
                                          .kind = TextureKind::Cube,
                                          .mipLevels = kLevels,
                                          .sampled = true,
                                          .label = "lmx.test.copy.cube"},
                                         mips);
    INFO(errorOf(cube));
    REQUIRE(cube.has_value());

    auto staging = (*device)->createBuffer({.size = kCapturedExtent * kCapturedExtent * 4,
                                            .cpuReadback = true,
                                            .label = "lmx.test.copy.cubeStaging"},
                                           nullptr);
    INFO(errorOf(staging));
    REQUIRE(staging.has_value());

    CommandList& commands = (*device)->beginFrame();
    commands.beginCopyPass("lmx.test.copy.cubeCapture");
    commands.copyTextureToBuffer(**cube,
                                 {.mipLevel = kCapturedLevel,
                                  .arrayLayer = kCapturedFace,
                                  .width = kCapturedExtent,
                                  .height = kCapturedExtent},
                                 **staging, {.bytesPerRow = kCapturedExtent * 4});
    commands.endCopyPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kCapturedExtent} * kCapturedExtent * 4, 0);
    (*staging)->readback(pixels.data(), pixels.size());

    const std::array<uint8_t, 4> expected = faceTexel(kCapturedFace, kCapturedLevel);
    for (size_t byte = 0; byte < pixels.size(); ++byte) {
        INFO("byte " + std::to_string(byte));
        REQUIRE(pixels[byte] == expected[byte % 4]);
    }
}

//======================================================================================================================
// The other direction, into a sub-rectangle of a rendered target: the pass clears the whole
// texture, the copy overwrites one interior rectangle, and both the rectangle and the untouched
// border are asserted, so an origin the copy ignored is visible as a displaced rectangle.
TEST_CASE("a copy writes buffer bytes into a texture sub-rectangle", "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint32_t kPatchOrigin = 8;
    constexpr uint32_t kPatchExtent = 16;
    // BGRA8Unorm, in the channel order readback() produces.
    constexpr uint8_t kPatchB = 0x10, kPatchG = 0x20, kPatchR = 0x30, kPatchA = 0xFF;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto target = makeProbeTarget(**device, "lmx.test.copy.patchTarget");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    std::vector<uint8_t> patch(size_t{kPatchExtent} * kPatchExtent * 4);
    for (size_t texel = 0; texel < size_t{kPatchExtent} * kPatchExtent; ++texel) {
        patch[texel * 4] = kPatchB;
        patch[texel * 4 + 1] = kPatchG;
        patch[texel * 4 + 2] = kPatchR;
        patch[texel * 4 + 3] = kPatchA;
    }
    auto staging = (*device)->createBuffer(
        {.size = patch.size(), .label = "lmx.test.copy.patchStaging"}, patch.data());
    INFO(errorOf(staging));
    REQUIRE(staging.has_value());

    CommandList& commands = (*device)->beginFrame();
    // A black clear, so every channel of the patch differs from the background it replaces.
    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.copy.patchClear"});
    commands.endRenderPass();

    commands.textureBarrier(**target, TextureUse::RenderTarget, TextureUse::CopyDestination);

    commands.beginCopyPass("lmx.test.copy.patchWrite");
    commands.copyBufferToTexture(
        **staging, {.bytesPerRow = kPatchExtent * 4}, **target,
        {.x = kPatchOrigin, .y = kPatchOrigin, .width = kPatchExtent, .height = kPatchExtent});
    commands.endCopyPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*target)->readback(pixels.data(), pixels.size());

    const std::array<std::pair<uint32_t, uint32_t>, 6> probes = {
        {{kPatchOrigin, kPatchOrigin},
         {kPatchOrigin + kPatchExtent - 1, kPatchOrigin + kPatchExtent - 1},
         {kPatchOrigin + 5, kPatchOrigin + 9},
         {kPatchOrigin - 1, kPatchOrigin},
         {kPatchOrigin + kPatchExtent, kPatchOrigin},
         {0, 0}}};
    for (const auto& [x, y] : probes) {
        const Pixel texel = pixelAt(pixels, x, y);
        INFO(describe("patched", x, y, texel));
        const bool inside = x >= kPatchOrigin && x < kPatchOrigin + kPatchExtent &&
                            y >= kPatchOrigin && y < kPatchOrigin + kPatchExtent;
        REQUIRE(texel.b == (inside ? kPatchB : 0));
        REQUIRE(texel.g == (inside ? kPatchG : 0));
        REQUIRE(texel.r == (inside ? kPatchR : 0));
        REQUIRE(texel.a == 255);
    }
}

//======================================================================================================================
// Texture to texture, of an offset rectangle into a different origin: the destination is compared
// against the same rectangle read back from the source, so the case asserts copy fidelity over
// rendered content rather than over a constant that would survive a mis-addressed copy.
TEST_CASE("a copy moves a rectangle between two textures", "[gpu]") {
    using namespace lmx::rhi;

    constexpr uint32_t kRegionOrigin = 16;
    constexpr uint32_t kRegionExtent = 32;

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
                                                       .cullMode = CullMode::None,
                                                       .label = "lmx.test.copy.regionPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto vertices = (*device)->createBuffer(
        {.size = sizeof(kTriangle), .label = "lmx.test.copy.regionVertices"}, kTriangle.data());
    INFO(errorOf(vertices));
    REQUIRE(vertices.has_value());

    auto source = makeProbeTarget(**device, "lmx.test.copy.regionSource");
    INFO(errorOf(source));
    REQUIRE(source.has_value());

    auto destination = (*device)->createTexture({.width = kSize,
                                                 .height = kSize,
                                                 .format = Format::BGRA8Unorm,
                                                 .cpuReadback = true,
                                                 .label = "lmx.test.copy.regionDestination"});
    INFO(errorOf(destination));
    REQUIRE(destination.has_value());

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass({.colorTarget = source->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.copy.regionDraw"});
    commands.bindPipeline(**pipeline);
    commands.bindBuffer(kVertexBufferSlot, **vertices);
    commands.draw(3);
    commands.endRenderPass();

    commands.textureBarrier(**source, TextureUse::RenderTarget, TextureUse::CopySource);

    commands.beginCopyPass("lmx.test.copy.region");
    commands.copyTexture(
        **source,
        {.x = kRegionOrigin, .y = kRegionOrigin, .width = kRegionExtent, .height = kRegionExtent},
        **destination, {.width = kRegionExtent, .height = kRegionExtent});
    commands.endCopyPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> sourcePixels(size_t{kSize} * kSize * 4);
    (*source)->readback(sourcePixels.data(), sourcePixels.size());
    std::vector<uint8_t> destinationPixels(size_t{kSize} * kSize * 4);
    (*destination)->readback(destinationPixels.data(), destinationPixels.size());

    // The rendered rectangle must not be uniform, or the comparison below would hold for a copy
    // that read the wrong offset.
    bool varied = false;
    for (uint32_t y = 0; y < kRegionExtent && !varied; ++y) {
        for (uint32_t x = 0; x < kRegionExtent; ++x) {
            const Pixel first = pixelAt(sourcePixels, kRegionOrigin, kRegionOrigin);
            const Pixel here = pixelAt(sourcePixels, kRegionOrigin + x, kRegionOrigin + y);
            if (here.r != first.r || here.g != first.g || here.b != first.b) {
                varied = true;
                break;
            }
        }
    }
    REQUIRE(varied);

    for (uint32_t y = 0; y < kRegionExtent; ++y) {
        for (uint32_t x = 0; x < kRegionExtent; ++x) {
            const Pixel expected = pixelAt(sourcePixels, kRegionOrigin + x, kRegionOrigin + y);
            const Pixel actual = pixelAt(destinationPixels, x, y);
            INFO(describe("copied", x, y, actual));
            REQUIRE(actual.b == expected.b);
            REQUIRE(actual.g == expected.g);
            REQUIRE(actual.r == expected.r);
            REQUIRE(actual.a == expected.a);
        }
    }
}

//======================================================================================================================
// A copy pass is timed by the same boundary timestamps the other two pass kinds use, so a frame
// mixing all three reports three passes in encode order.
TEST_CASE("pass timings cover a copy pass alongside the other pass kinds", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/ComputeImageSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createComputePipeline(
        {.library = library->get(),
         .computeEntry = "computeWriteImage",
         .threadsPerThreadgroup = {kCopyImageThreadsPerGroup, kCopyImageThreadsPerGroup, 1},
         .label = "lmx.test.copy.timingPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto image = (*device)->createTexture({.width = kSize,
                                           .height = kSize,
                                           .format = Format::RGBA8Unorm,
                                           .storageWrite = true,
                                           .label = "lmx.test.copy.timingImage"});
    INFO(errorOf(image));
    REQUIRE(image.has_value());

    auto target = makeProbeTarget(**device, "lmx.test.copy.timingTarget");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    auto staging = (*device)->createBuffer(
        {.size = 256, .cpuReadback = true, .label = "lmx.test.copy.timingStaging"}, nullptr);
    INFO(errorOf(staging));
    REQUIRE(staging.has_value());

    const CopyImageParams params{.extent = kSize};

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.timing.render"});
    commands.endRenderPass();
    commands.beginComputePass("lmx.test.timing.compute");
    commands.bindComputePipeline(**pipeline);
    commands.bindStorageTexture(kCopyImageSlot, **image, {}, StorageAccess::Write);
    commands.bindFrameData(kCopyImageParamsSlot, params);
    commands.dispatch(kSize / kCopyImageThreadsPerGroup, kSize / kCopyImageThreadsPerGroup, 1);
    commands.endComputePass();
    commands.beginCopyPass("lmx.test.timing.copy");
    commands.fillBuffer(**staging, 0, 256, 0x11);
    commands.endCopyPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    // waitIdle retires the measured frame; the next beginFrame is what publishes its counters.
    (*device)->beginFrame();
    (*device)->endFrame(nullptr);

    const std::span<const PassTiming> timings = (*device)->passTimings();
    REQUIRE(timings.size() == 3);
    REQUIRE(timings[0].label == "lmx.test.timing.render");
    REQUIRE(timings[1].label == "lmx.test.timing.compute");
    REQUIRE(timings[2].label == "lmx.test.timing.copy");
    REQUIRE((*device)->passTimingsFrame() == 1);
}

//======================================================================================================================
// The shipped frame can exceed sixteen passes when bloom and auto-exposure are enabled together.
// Empty compute encoders isolate the timestamp capacity contract from shader or resource work.
TEST_CASE("pass timings retain more than sixteen passes", "[gpu]") {
    using namespace lmx::rhi;

    constexpr uint32_t kPassCount = 20;
    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    CommandList& commands = (*device)->beginFrame();
    for (uint32_t pass = 0; pass < kPassCount; ++pass) {
        commands.beginComputePass("lmx.test.timing.many." + std::to_string(pass));
        commands.endComputePass();
    }
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    (*device)->beginFrame();
    (*device)->endFrame(nullptr);

    const std::span<const PassTiming> timings = (*device)->passTimings();
    REQUIRE(timings.size() == kPassCount);
    for (uint32_t pass = 0; pass < kPassCount; ++pass) {
        REQUIRE(timings[pass].label == "lmx.test.timing.many." + std::to_string(pass));
    }
}
