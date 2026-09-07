#include "GpuTestSupport.h"

#include "RHI/Validate.h"

#include <cstring>
#include <utility>

namespace {

// The two halves MrtSmoke.slang writes, as the bits an RG16Float readback produces: 0.25 is
// 2^-2 and -0.5 is 2^-1, both exact in half precision, so the comparison is bit for bit.
constexpr uint16_t kWrittenMotionX = 0x3400;
constexpr uint16_t kWrittenMotionY = 0xB800;

// The clear the motion attachment carries where nothing rasterised, chosen exact for the same
// reason: -1.0 and 2.0.
constexpr float kMotionClearX = -1.0f;
constexpr float kMotionClearY = 2.0f;
constexpr uint16_t kClearedMotionX = 0xBC00;
constexpr uint16_t kClearedMotionY = 0x4000;

// One RG16Float texel, in the channel order readback() produces.
struct MotionTexel {
    uint16_t r = 0;
    uint16_t g = 0;
};

//======================================================================================================================
MotionTexel motionAt(const std::vector<uint8_t>& bytes, uint32_t x, uint32_t y) {
    MotionTexel texel{};
    std::memcpy(&texel, bytes.data() + (size_t{y} * kSize + x) * sizeof(MotionTexel),
                sizeof(MotionTexel));
    return texel;
}

//======================================================================================================================
std::string describeMotion(uint32_t x, uint32_t y, const MotionTexel& texel) {
    return "motion (" + std::to_string(x) + "," + std::to_string(y) +
           "): R=" + std::to_string(texel.r) + " G=" + std::to_string(texel.g);
}

//======================================================================================================================
lmx::rhi::Result<std::unique_ptr<lmx::rhi::Texture>> makeMotionTarget(lmx::rhi::Device& device,
                                                                      const char* label) {
    return device.createTexture({.width = kSize,
                                 .height = kSize,
                                 .format = lmx::rhi::Format::RG16Float,
                                 .renderTarget = true,
                                 .cpuReadback = true,
                                 .label = label});
}

} // namespace

//======================================================================================================================
// One pass, two attachments: the fragment stage writes both, and each readback has to show its own
// value rather than the other attachment's or its own clear.
TEST_CASE("a render pass writes an extra color attachment alongside the primary", "[gpu][rhi]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto color = makeProbeTarget(**device, "lmx.test.mrtColor");
    INFO(errorOf(color));
    REQUIRE(color.has_value());

    auto motion = makeMotionTarget(**device, "lmx.test.mrtMotion");
    INFO(errorOf(motion));
    REQUIRE(motion.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/MrtSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = (*device)->createGraphicsPipeline(
        {.library = library->get(),
         .vertexEntry = "vertexMain",
         .fragmentEntry = "fragmentMain",
         .colorFormat = Format::BGRA8Unorm,
         .extraColorFormats = {Format::RG16Float, Format::Unknown, Format::Unknown},
         .extraColorCount = 1,
         .label = "lmx.test.mrtPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass(
        {.colorTarget = color->get(),
         .clearColor = {0.0f, 1.0f, 0.0f, 1.0f},
         .clear = true,
         .extraColor = {{.target = motion->get(),
                         .clearColor = {kMotionClearX, kMotionClearY, 0.0f, 0.0f},
                         .clear = true}},
         .extraColorCount = 1,
         .label = "lmx.test.mrt"});
    commands.bindPipeline(**pipeline);
    commands.draw(3);
    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> colorPixels(size_t{kSize} * kSize * 4);
    (*color)->readback(colorPixels.data(), colorPixels.size());
    std::vector<uint8_t> motionPixels(size_t{kSize} * kSize * sizeof(MotionTexel));
    (*motion)->readback(motionPixels.data(), motionPixels.size());

    // The fullscreen triangle covers every texel, so both corners and the centre carry the write.
    for (const std::pair<uint32_t, uint32_t> probe :
         {std::pair<uint32_t, uint32_t>{1, 1}, {kSize / 2, kSize / 2}, {kSize - 2, kSize - 2}}) {
        const Pixel pixel = pixelAt(colorPixels, probe.first, probe.second);
        INFO(describe("color", probe.first, probe.second, pixel));
        REQUIRE(pixel.r == 255);
        REQUIRE(pixel.g == 0);
        REQUIRE(pixel.b == 0);
        REQUIRE(pixel.a == 255);

        const MotionTexel texel = motionAt(motionPixels, probe.first, probe.second);
        INFO(describeMotion(probe.first, probe.second, texel));
        REQUIRE(texel.r == kWrittenMotionX);
        REQUIRE(texel.g == kWrittenMotionY);
    }
}

//======================================================================================================================
// The same pass without a draw. Its only output is the load action, so this is what proves the
// extra attachment's own clear colour reaches it instead of the primary's.
TEST_CASE("an extra color attachment clears to its own value", "[gpu][rhi]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto color = makeProbeTarget(**device, "lmx.test.mrtClearColor");
    INFO(errorOf(color));
    REQUIRE(color.has_value());

    auto motion = makeMotionTarget(**device, "lmx.test.mrtClearMotion");
    INFO(errorOf(motion));
    REQUIRE(motion.has_value());

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass(
        {.colorTarget = color->get(),
         .clearColor = {0.0f, 0.0f, 1.0f, 1.0f},
         .clear = true,
         .extraColor = {{.target = motion->get(),
                         .clearColor = {kMotionClearX, kMotionClearY, 0.0f, 0.0f},
                         .clear = true}},
         .extraColorCount = 1,
         .label = "lmx.test.mrtClear"});
    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> colorPixels(size_t{kSize} * kSize * 4);
    (*color)->readback(colorPixels.data(), colorPixels.size());
    std::vector<uint8_t> motionPixels(size_t{kSize} * kSize * sizeof(MotionTexel));
    (*motion)->readback(motionPixels.data(), motionPixels.size());

    const Pixel pixel = pixelAt(colorPixels, kSize / 2, kSize / 2);
    INFO(describe("color", kSize / 2, kSize / 2, pixel));
    REQUIRE(pixel.b == 255);
    REQUIRE(pixel.g == 0);
    REQUIRE(pixel.r == 0);

    const MotionTexel texel = motionAt(motionPixels, kSize / 2, kSize / 2);
    INFO(describeMotion(kSize / 2, kSize / 2, texel));
    REQUIRE(texel.r == kClearedMotionX);
    REQUIRE(texel.g == kClearedMotionY);
}

//======================================================================================================================
// Attachment zero is the primary colour target, so a pass that has none cannot have an attachment
// one either. Checked against real device textures rather than the unit test's stand-ins.
TEST_CASE("a depth-only pass rejects extra color attachments", "[gpu][rhi]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto motion = makeMotionTarget(**device, "lmx.test.mrtDepthOnlyMotion");
    INFO(errorOf(motion));
    REQUIRE(motion.has_value());

    const ExtraColorTarget extras[1] = {{.target = motion->get()}};

    const auto r = validateExtraColorTargets(nullptr, extras, 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("colorTarget"));
}
