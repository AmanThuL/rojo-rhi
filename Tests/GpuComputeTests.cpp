#include "GpuTestSupport.h"

#include <limits>

namespace {

// Mirrors ComputeSmoke.slang's ComputeParams; the layout is pinned by the shader, not chosen here.
struct ComputeParams {
    uint32_t bias = 0;
    uint32_t extent = 0;
};

// Mirrors ComputeImageSmoke.slang's ImageParams.
struct ImageParams {
    uint32_t extent = 0;
};

// computeFillBuffer's [numthreads]. The RHI takes threadgroup counts, so every dispatch below
// derives its count from this.
constexpr uint32_t kFillThreadsPerGroup = 64;

// The image kernels' [numthreads] in x and y.
constexpr uint32_t kImageThreadsPerGroup = 8;

// The argument-table slots ComputeImageSmoke.slang's globals compile to.
constexpr uint32_t kImageSlot = 0;
constexpr uint32_t kStorageSourceSlot = 1;
constexpr uint32_t kParamsSlot = 1;

//======================================================================================================================
// The gradient computeWriteImage writes, as the 8-bit channel value at a texel of a square image
// of the given extent -- the extent the kernel was given, which for a mip view is that mip's.
uint8_t gradientChannel(uint32_t coordinate, uint32_t extent = kSize) {
    const float value = static_cast<float>(coordinate) / static_cast<float>(extent - 1);
    return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

} // namespace

//======================================================================================================================
// The element values are distinct arithmetic results rather than a constant, so a dispatch that
// writes the wrong index, skips a threadgroup, or never runs at all fails on a specific element.
TEST_CASE("a dispatch fills a storage buffer the CPU reads back", "[gpu]") {
    using namespace lmx::rhi;

    constexpr uint32_t kElements = 256;
    constexpr uint32_t kBias = 11;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/ComputeSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline =
        (*device)->createComputePipeline({.library = library->get(),
                                          .computeEntry = "computeFillBuffer",
                                          .threadsPerThreadgroup = {kFillThreadsPerGroup, 1, 1},
                                          .label = "lmx.test.compute.fillPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto storage = (*device)->createBuffer({.size = sizeof(uint32_t) * kElements,
                                            .storageWrite = true,
                                            .cpuReadback = true,
                                            .label = "lmx.test.compute.fillStorage"},
                                           nullptr);
    INFO(errorOf(storage));
    REQUIRE(storage.has_value());

    const ComputeParams params{.bias = kBias, .extent = 0};

    CommandList& commands = (*device)->beginFrame();
    commands.beginComputePass("lmx.test.compute.fill");
    commands.bindComputePipeline(**pipeline);
    commands.bindStorageBuffer(0, **storage, StorageAccess::Write);
    commands.bindFrameData(1, params);
    commands.dispatch(kElements / kFillThreadsPerGroup, 1, 1);
    commands.endComputePass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint32_t> values(kElements, 0);
    (*storage)->readback(values.data(), values.size() * sizeof(uint32_t));

    for (uint32_t index = 0; index < kElements; ++index) {
        INFO("element " + std::to_string(index));
        REQUIRE(values[index] == index * 3 + kBias);
    }
}

//======================================================================================================================
// The threadgroup shape is host-supplied. Reject an impossible per-axis size during creation rather
// than letting it reach a later dispatch, where Metal reports only an encoder validation failure.
TEST_CASE("a compute pipeline rejects a threadgroup dimension beyond the device limit", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/ComputeSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createComputePipeline(
        {.library = library->get(),
         .computeEntry = "computeFillBuffer",
         .threadsPerThreadgroup = {std::numeric_limits<uint32_t>::max(), 1, 1},
         .label = "lmx.test.compute.invalidThreadgroup"});
    REQUIRE_FALSE(pipeline.has_value());
    REQUIRE(pipeline.error().code == ErrorCode::PipelineCreationFailed);
    REQUIRE(pipeline.error().message.contains("per-axis limit"));
}

//======================================================================================================================
// The gradient is written by a compute kernel and read straight back, so the case pins the storage
// binding itself: usage, texel addressing, and channel order, with no second pass in between.
TEST_CASE("a dispatch writes a storage texture the CPU reads back", "[gpu]") {
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
         .threadsPerThreadgroup = {kImageThreadsPerGroup, kImageThreadsPerGroup, 1},
         .label = "lmx.test.compute.writeImagePipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto image = (*device)->createTexture({.width = kSize,
                                           .height = kSize,
                                           .format = Format::RGBA8Unorm,
                                           .storageWrite = true,
                                           .cpuReadback = true,
                                           .label = "lmx.test.compute.image"});
    INFO(errorOf(image));
    REQUIRE(image.has_value());

    const ImageParams params{.extent = kSize};

    CommandList& commands = (*device)->beginFrame();
    commands.beginComputePass("lmx.test.compute.writeImage");
    commands.bindComputePipeline(**pipeline);
    commands.bindStorageTexture(kImageSlot, **image, {}, StorageAccess::Write);
    commands.bindFrameData(kParamsSlot, params);
    commands.dispatch(kSize / kImageThreadsPerGroup, kSize / kImageThreadsPerGroup, 1);
    commands.endComputePass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*image)->readback(pixels.data(), pixels.size());

    // Corners and one interior texel: enough to catch a transposed, flipped, or constant write.
    const std::array<std::pair<uint32_t, uint32_t>, 4> probes = {
        {{0, 0}, {kSize - 1, 0}, {0, kSize - 1}, {21, 42}}};
    for (const auto& [x, y] : probes) {
        const size_t offset = (size_t{y} * kSize + x) * 4;
        INFO("texel (" + std::to_string(x) + "," + std::to_string(y) + ")");
        REQUIRE(pixels[offset] == gradientChannel(x));
        REQUIRE(pixels[offset + 1] == gradientChannel(y));
        REQUIRE(channelNear(pixels[offset + 2], 64, 1));
        REQUIRE(pixels[offset + 3] == 255);
    }
}

//======================================================================================================================
// Two dispatches in one frame with a storage-write to storage-read barrier between them. Without
// the barrier the second pass may read the source before the first has written it; the complement
// the reader writes is what makes a stale read (the original gradient) distinguishable.
TEST_CASE("a storage texture written by one dispatch is read by the next", "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/ComputeImageSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto writePipeline = (*device)->createComputePipeline(
        {.library = library->get(),
         .computeEntry = "computeWriteImage",
         .threadsPerThreadgroup = {kImageThreadsPerGroup, kImageThreadsPerGroup, 1},
         .label = "lmx.test.compute.hazardWritePipeline"});
    INFO(errorOf(writePipeline));
    REQUIRE(writePipeline.has_value());

    auto invertPipeline = (*device)->createComputePipeline(
        {.library = library->get(),
         .computeEntry = "computeInvertImage",
         .threadsPerThreadgroup = {kImageThreadsPerGroup, kImageThreadsPerGroup, 1},
         .label = "lmx.test.compute.hazardInvertPipeline"});
    INFO(errorOf(invertPipeline));
    REQUIRE(invertPipeline.has_value());

    auto source = (*device)->createTexture({.width = kSize,
                                            .height = kSize,
                                            .format = Format::RGBA8Unorm,
                                            .storageRead = true,
                                            .storageWrite = true,
                                            .label = "lmx.test.compute.hazardSource"});
    INFO(errorOf(source));
    REQUIRE(source.has_value());

    auto destination = (*device)->createTexture({.width = kSize,
                                                 .height = kSize,
                                                 .format = Format::RGBA8Unorm,
                                                 .storageWrite = true,
                                                 .cpuReadback = true,
                                                 .label = "lmx.test.compute.hazardDestination"});
    INFO(errorOf(destination));
    REQUIRE(destination.has_value());

    const ImageParams params{.extent = kSize};
    const uint32_t groups = kSize / kImageThreadsPerGroup;

    CommandList& commands = (*device)->beginFrame();
    commands.beginComputePass("lmx.test.compute.hazardWrite");
    commands.bindComputePipeline(**writePipeline);
    commands.bindStorageTexture(kImageSlot, **source, {}, StorageAccess::Write);
    commands.bindFrameData(kParamsSlot, params);
    commands.dispatch(groups, groups, 1);
    commands.endComputePass();

    commands.textureBarrier(**source, TextureUse::StorageWrite, TextureUse::StorageRead);

    commands.beginComputePass("lmx.test.compute.hazardRead");
    commands.bindComputePipeline(**invertPipeline);
    commands.bindStorageTexture(kImageSlot, **destination, {}, StorageAccess::Write);
    commands.bindStorageTexture(kStorageSourceSlot, **source, {}, StorageAccess::Read);
    commands.bindFrameData(kParamsSlot, params);
    commands.dispatch(groups, groups, 1);
    commands.endComputePass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*destination)->readback(pixels.data(), pixels.size());

    const std::array<std::pair<uint32_t, uint32_t>, 3> probes = {
        {{0, 0}, {kSize - 1, 0}, {21, 42}}};
    for (const auto& [x, y] : probes) {
        const size_t offset = (size_t{y} * kSize + x) * 4;
        INFO("texel (" + std::to_string(x) + "," + std::to_string(y) + ")");
        REQUIRE(channelNear(pixels[offset], 255 - gradientChannel(x), 1));
        REQUIRE(channelNear(pixels[offset + 1], 255 - gradientChannel(y), 1));
        REQUIRE(channelNear(pixels[offset + 2], 255 - 64, 1));
        REQUIRE(pixels[offset + 3] == 255);
    }
}

//======================================================================================================================
// The compute-to-sample hazard: a dispatch writes a texture and a fragment shader in the next pass
// reads it. The two run on different queue stages, so the barrier between them is the only thing
// making the write visible to the read.
TEST_CASE("a storage texture written by a dispatch is sampled by a later draw",
          "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto computeLibrary = (*device)->loadShaderLibrary("Shaders/ComputeImageSmoke");
    INFO(errorOf(computeLibrary));
    REQUIRE(computeLibrary.has_value());

    auto pipeline = (*device)->createComputePipeline(
        {.library = computeLibrary->get(),
         .computeEntry = "computeWriteImage",
         .threadsPerThreadgroup = {kImageThreadsPerGroup, kImageThreadsPerGroup, 1},
         .label = "lmx.test.compute.samplePipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto sampleLibrary = (*device)->loadShaderLibrary("Shaders/FullscreenSample");
    INFO(errorOf(sampleLibrary));
    REQUIRE(sampleLibrary.has_value());

    auto samplePipeline =
        (*device)->createGraphicsPipeline({.library = sampleLibrary->get(),
                                           .vertexEntry = "vertexMain",
                                           .fragmentEntry = "fragmentMain",
                                           .colorFormat = Format::BGRA8Unorm,
                                           .label = "lmx.test.compute.sampleDrawPipeline"});
    INFO(errorOf(samplePipeline));
    REQUIRE(samplePipeline.has_value());

    auto image = (*device)->createTexture({.width = kSize,
                                           .height = kSize,
                                           .format = Format::RGBA8Unorm,
                                           .sampled = true,
                                           .storageWrite = true,
                                           .label = "lmx.test.compute.sampledImage"});
    INFO(errorOf(image));
    REQUIRE(image.has_value());

    auto target = makeProbeTarget(**device, "lmx.test.compute.sampleTarget");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    const ImageParams params{.extent = kSize};
    const uint32_t groups = kSize / kImageThreadsPerGroup;

    CommandList& commands = (*device)->beginFrame();
    commands.beginComputePass("lmx.test.compute.sampleWrite");
    commands.bindComputePipeline(**pipeline);
    commands.bindStorageTexture(kImageSlot, **image, {}, StorageAccess::Write);
    commands.bindFrameData(kParamsSlot, params);
    commands.dispatch(groups, groups, 1);
    commands.endComputePass();

    commands.textureBarrier(**image, TextureUse::StorageWrite, TextureUse::ShaderRead);

    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {1.0f, 0.0f, 1.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.compute.sampleDraw"});
    commands.bindPipeline(**samplePipeline);
    commands.bindTexture(0, **image);
    commands.draw(3);
    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*target)->readback(pixels.data(), pixels.size());

    const std::array<std::pair<uint32_t, uint32_t>, 3> probes = {
        {{0, 0}, {kSize - 1, 0}, {21, 42}}};
    for (const auto& [x, y] : probes) {
        const Pixel texel = pixelAt(pixels, x, y);
        INFO(describe("sampled", x, y, texel));
        REQUIRE(channelNear(texel.r, gradientChannel(x), 1));
        REQUIRE(channelNear(texel.g, gradientChannel(y), 1));
        REQUIRE(channelNear(texel.b, 64, 1));
        REQUIRE(texel.a == 255);
    }
}

//======================================================================================================================
// Both levels of a two-level chain are written through views, each with its own extent, and then
// read back through views. Level 0 keeping its own gradient is what proves the level-1 write landed
// where it was aimed: a view that silently resolved to level 0 would have overwritten level 0's
// top-left quadrant with the level-1 gradient, whose values differ at every interior texel.
TEST_CASE("a storage texture view addresses a single mip level", "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint32_t kMipExtent = kSize / 2;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/ComputeImageSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto writePipeline = (*device)->createComputePipeline(
        {.library = library->get(),
         .computeEntry = "computeWriteImage",
         .threadsPerThreadgroup = {kImageThreadsPerGroup, kImageThreadsPerGroup, 1},
         .label = "lmx.test.compute.mipWritePipeline"});
    INFO(errorOf(writePipeline));
    REQUIRE(writePipeline.has_value());

    auto invertPipeline = (*device)->createComputePipeline(
        {.library = library->get(),
         .computeEntry = "computeInvertImage",
         .threadsPerThreadgroup = {kImageThreadsPerGroup, kImageThreadsPerGroup, 1},
         .label = "lmx.test.compute.mipInvertPipeline"});
    INFO(errorOf(invertPipeline));
    REQUIRE(invertPipeline.has_value());

    auto chain = (*device)->createTexture({.width = kSize,
                                           .height = kSize,
                                           .format = Format::RGBA8Unorm,
                                           .mipLevels = 2,
                                           .storageRead = true,
                                           .storageWrite = true,
                                           .label = "lmx.test.compute.mipChain"});
    INFO(errorOf(chain));
    REQUIRE(chain.has_value());

    const auto makeDestination = [&](uint32_t extent, const char* label) {
        return (*device)->createTexture({.width = extent,
                                         .height = extent,
                                         .format = Format::RGBA8Unorm,
                                         .storageWrite = true,
                                         .cpuReadback = true,
                                         .label = label});
    };
    auto level0Destination = makeDestination(kSize, "lmx.test.compute.mipLevel0Destination");
    INFO(errorOf(level0Destination));
    REQUIRE(level0Destination.has_value());
    auto level1Destination = makeDestination(kMipExtent, "lmx.test.compute.mipLevel1Destination");
    INFO(errorOf(level1Destination));
    REQUIRE(level1Destination.has_value());

    const TextureViewDesc level0{.range = {.baseMipLevel = 0, .mipLevelCount = 1}};
    const TextureViewDesc level1{.range = {.baseMipLevel = 1, .mipLevelCount = 1}};
    const ImageParams level0Params{.extent = kSize};
    const ImageParams level1Params{.extent = kMipExtent};

    CommandList& commands = (*device)->beginFrame();
    const auto writeLevel = [&](const char* label, const TextureViewDesc& view,
                                const ImageParams& params, uint32_t extent) {
        commands.beginComputePass(label);
        commands.bindComputePipeline(**writePipeline);
        commands.bindStorageTexture(kImageSlot, **chain, view, StorageAccess::Write);
        commands.bindFrameData(kParamsSlot, params);
        commands.dispatch(extent / kImageThreadsPerGroup, extent / kImageThreadsPerGroup, 1);
        commands.endComputePass();
    };
    // The two writes touch disjoint levels, so they need no barrier between them -- only the reads
    // below depend on either.
    writeLevel("lmx.test.compute.mipWrite0", level0, level0Params, kSize);
    writeLevel("lmx.test.compute.mipWrite1", level1, level1Params, kMipExtent);

    commands.textureBarrier(**chain, TextureUse::StorageWrite, TextureUse::StorageRead);

    const auto readLevel = [&](const char* label, const TextureViewDesc& view, Texture& destination,
                               const ImageParams& params, uint32_t extent) {
        commands.beginComputePass(label);
        commands.bindComputePipeline(**invertPipeline);
        commands.bindStorageTexture(kImageSlot, destination, {}, StorageAccess::Write);
        commands.bindStorageTexture(kStorageSourceSlot, **chain, view, StorageAccess::Read);
        commands.bindFrameData(kParamsSlot, params);
        commands.dispatch(extent / kImageThreadsPerGroup, extent / kImageThreadsPerGroup, 1);
        commands.endComputePass();
    };
    readLevel("lmx.test.compute.mipRead1", level1, **level1Destination, level1Params, kMipExtent);
    readLevel("lmx.test.compute.mipRead0", level0, **level0Destination, level0Params, kSize);
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    const auto probeLevel = [](Texture& destination, uint32_t extent) {
        std::vector<uint8_t> pixels(size_t{extent} * extent * 4);
        destination.readback(pixels.data(), pixels.size());
        // Interior texels of the level-1 quadrant, where the two levels' gradients disagree most.
        const std::array<std::pair<uint32_t, uint32_t>, 3> probes = {
            {{1, 1}, {extent / 4, extent / 4}, {11, 20}}};
        for (const auto& [x, y] : probes) {
            const size_t offset = (size_t{y} * extent + x) * 4;
            INFO("level of extent " + std::to_string(extent) + ", texel (" + std::to_string(x) +
                 "," + std::to_string(y) + ")");
            REQUIRE(channelNear(pixels[offset], 255 - gradientChannel(x, extent), 1));
            REQUIRE(channelNear(pixels[offset + 1], 255 - gradientChannel(y, extent), 1));
            REQUIRE(pixels[offset + 3] == 255);
        }
    };
    probeLevel(**level1Destination, kMipExtent);
    probeLevel(**level0Destination, kSize);
}

//======================================================================================================================
// A frame with one pass of each kind: both are timed, in encode order, and the publication names
// the frame it measured rather than leaving the caller to guess how far the readout trails.
TEST_CASE("pass timings cover a compute pass and name the frame they measured", "[gpu]") {
    using namespace lmx::rhi;

    constexpr uint32_t kElements = 64;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());
    REQUIRE((*device)->passTimingsFrame() == 0);

    auto library = (*device)->loadShaderLibrary("Shaders/ComputeSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline =
        (*device)->createComputePipeline({.library = library->get(),
                                          .computeEntry = "computeFillBuffer",
                                          .threadsPerThreadgroup = {kFillThreadsPerGroup, 1, 1},
                                          .label = "lmx.test.compute.timingPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto storage = (*device)->createBuffer({.size = sizeof(uint32_t) * kElements,
                                            .storageWrite = true,
                                            .label = "lmx.test.compute.timingStorage"},
                                           nullptr);
    INFO(errorOf(storage));
    REQUIRE(storage.has_value());

    auto target = makeProbeTarget(**device, "lmx.test.compute.timingTarget");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    const ComputeParams params{.bias = 1, .extent = 0};

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.timing.render"});
    commands.endRenderPass();
    commands.beginComputePass("lmx.test.timing.compute");
    commands.bindComputePipeline(**pipeline);
    commands.bindStorageBuffer(0, **storage, StorageAccess::Write);
    commands.bindFrameData(1, params);
    commands.dispatch(kElements / kFillThreadsPerGroup, 1, 1);
    commands.endComputePass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    // waitIdle retires the measured frame; the next beginFrame is what publishes its counters.
    (*device)->beginFrame();
    (*device)->endFrame(nullptr);

    const std::span<const PassTiming> timings = (*device)->passTimings();
    REQUIRE(timings.size() == 2);
    REQUIRE(timings[0].label == "lmx.test.timing.render");
    REQUIRE(timings[1].label == "lmx.test.timing.compute");
    REQUIRE(timings[1].gpuMilliseconds > 0.0);
    // The measured frame is the first one this device opened, whatever the readout lag.
    REQUIRE((*device)->passTimingsFrame() == 1);
}
