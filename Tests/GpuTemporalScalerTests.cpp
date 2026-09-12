#include "GpuTestSupport.h"

#include <algorithm>
#include <cmath>

//======================================================================================================================
TEST_CASE("vendor temporal scaler encodes, times and retires overlapping frames",
          "[gpu][rhi][vendor]") {
    using namespace lmx::rhi;
    auto deviceResult = createDevice();
    INFO(errorOf(deviceResult));
    REQUIRE(deviceResult);
    auto& device = **deviceResult;
    const auto& support = device.capabilities().temporalScaler;
    if (!support.available) {
        SKIP("device has no vendor temporal scaler capability");
    }
    REQUIRE_FALSE(support.name.empty());
    REQUIRE(support.minInputScale > 0.0f);
    REQUIRE(support.maxInputScale >= support.minInputScale);
    constexpr uint32_t width = 320, height = 180;
    TemporalScalerDesc desc{.inputWidth = width,
                            .inputHeight = height,
                            .outputWidth = width,
                            .outputHeight = height,
                            .minInputScale = 0.5f,
                            .maxInputScale = 1.0f,
                            .label = "lmx.test.temporalScaler"};
    if (support.minInputScale > desc.minInputScale || support.maxInputScale < desc.maxInputScale) {
        SKIP("device temporal scaler does not support test scale range");
    }
    auto scaler = device.createTemporalScaler(desc);
    INFO(errorOf(scaler));
    REQUIRE(scaler);
    auto color = device.createTexture({.width = width,
                                       .height = height,
                                       .format = Format::RGBA16Float,
                                       .renderTarget = true,
                                       .sampled = true,
                                       .label = "lmx.test.vendor.color"});
    auto depth = device.createTexture({.width = width,
                                       .height = height,
                                       .format = Format::D32Float,
                                       .renderTarget = true,
                                       .sampled = true,
                                       .label = "lmx.test.vendor.depth"});
    auto motion = device.createTexture({.width = width,
                                        .height = height,
                                        .format = Format::RG16Float,
                                        .renderTarget = true,
                                        .sampled = true,
                                        .label = "lmx.test.vendor.motion"});
    auto reactive = device.createTexture({.width = width,
                                          .height = height,
                                          .format = Format::R8Unorm,
                                          .renderTarget = true,
                                          .sampled = true,
                                          .label = "lmx.test.vendor.reactive"});
    const uint16_t oneHalf = 0x3c00;
    const TextureMip exposureMip{.data = &oneHalf, .bytesPerRow = 2};
    auto exposure = device.createTexture({.width = 1,
                                          .height = 1,
                                          .format = Format::R16Float,
                                          .sampled = true,
                                          .label = "lmx.test.vendor.exposure"},
                                         std::span{&exposureMip, 1});
    REQUIRE(color);
    REQUIRE(depth);
    REQUIRE(motion);
    REQUIRE(reactive);
    REQUIRE(exposure);
    bool sharedOutput = false;
    SECTION("private output") {
        sharedOutput = false;
    }
    SECTION("CPU-readable output") {
        sharedOutput = true;
    }
    auto output = device.createTexture({.width = width,
                                        .height = height,
                                        .format = Format::RGBA16Float,
                                        .renderTarget = true,
                                        .sampled = true,
                                        .storageWrite = true,
                                        .cpuReadback = sharedOutput,
                                        .label = "lmx.test.vendor.output"});
    REQUIRE(output);
    constexpr uint64_t byteCount = uint64_t{width} * height * 8;
    std::array<std::unique_ptr<Buffer>, 5> readbacks;
    for (auto& readback : readbacks) {
        auto made = device.createBuffer(
            {.size = byteCount, .cpuReadback = true, .label = "lmx.test.vendor.readback"}, nullptr);
        REQUIRE(made);
        readback = std::move(*made);
    }
    for (uint32_t frame = 0; frame < readbacks.size(); ++frame) {
        auto& commands = device.beginFrame();
        if (frame > 0) {
            commands.textureBarrier(**color, TextureUse::ExternalRead, TextureUse::RenderTarget);
            commands.textureBarrier(**depth, TextureUse::ExternalRead, TextureUse::RenderTarget);
        }
        commands.beginRenderPass({.colorTarget = color->get(),
                                  .clearColor = {0.25f, 0.5f, 0.75f, 1.0f},
                                  .clear = true,
                                  .depthTarget = depth->get(),
                                  .clearDepth = 0.5f,
                                  .storeDepth = true,
                                  .label = "lmx.test.vendor.clearScene"});
        commands.endRenderPass();
        if (frame > 0) {
            commands.textureBarrier(**motion, TextureUse::ExternalRead, TextureUse::RenderTarget);
        }
        commands.beginRenderPass({.colorTarget = motion->get(),
                                  .clearColor = {0, 0, 0, 0},
                                  .clear = true,
                                  .label = "lmx.test.vendor.clearMotion"});
        commands.endRenderPass();
        if (frame > 0) {
            commands.textureBarrier(**reactive, TextureUse::ExternalRead, TextureUse::RenderTarget);
        }
        commands.beginRenderPass({.colorTarget = reactive->get(),
                                  .clearColor = {0, 0, 0, 0},
                                  .clear = true,
                                  .label = "lmx.test.vendor.clearReactive"});
        commands.endRenderPass();
        for (Texture* input : {color->get(), depth->get(), motion->get(), reactive->get()}) {
            commands.textureBarrier(*input, TextureUse::RenderTarget, TextureUse::ExternalRead);
        }
        if (frame > 0) {
            commands.textureBarrier(**output, TextureUse::CopySource, TextureUse::ExternalWrite);
        }
        const uint32_t divisor = frame % 2 == 0 ? 2 : 1;
        commands.temporalScale(**scaler, {.color = color->get(),
                                          .depth = depth->get(),
                                          .motion = motion->get(),
                                          .reactive = reactive->get(),
                                          .exposure = exposure->get(),
                                          .output = output->get(),
                                          .inputContentWidth = width / divisor,
                                          .inputContentHeight = height / divisor,
                                          .reset = frame == 0,
                                          .label = "lmx.test.vendor.encode"});
        if (frame % 2 == 0) {
            // An unrelated pass still consumes the vendor fence even without a resource barrier.
            commands.beginComputePass("lmx.test.vendor.unrelated");
            commands.endComputePass();
        }
        commands.textureBarrier(**output, TextureUse::ExternalWrite, TextureUse::CopySource);
        commands.beginCopyPass("lmx.test.vendor.readback");
        commands.copyTextureToBuffer(**output, {.width = width, .height = height},
                                     *readbacks[frame], {.bytesPerRow = width * 8});
        commands.endCopyPass();
        if (frame == readbacks.size() - 1) {
            scaler->reset();
        }
        device.endFrame(nullptr);
    }
    device.waitIdle();
    std::vector<uint16_t> pixels(width * height * 4);
    for (auto& readback : readbacks) {
        readback->readback(pixels.data(), byteCount);
        const size_t middle = (height / 2 * width + width / 2) * 4;
        REQUIRE(pixels[middle] > 0);
        REQUIRE((pixels[middle] & 0x7c00) != 0x7c00);
        REQUIRE(pixels[middle + 1] > pixels[middle]);
        REQUIRE(pixels[middle + 2] > pixels[middle + 1]);
    }
    device.beginFrame();
    const auto timings = device.passTimings();
    auto vendor = std::ranges::find(timings, "lmx.test.vendor.encode", &PassTiming::label);
    REQUIRE(vendor != timings.end());
    REQUIRE(std::isfinite(vendor->gpuMilliseconds));
    REQUIRE(vendor->gpuMilliseconds >= 0.0);
    device.endFrame(nullptr);
    device.waitIdle();
}
