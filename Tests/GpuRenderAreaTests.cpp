#include "GpuTestSupport.h"

#include "RHI/Validate.h"

namespace {

// The sub-rectangle the render-area cases draw into, deliberately non-square so a backend that
// swapped width and height would paint the wrong texels.
constexpr uint32_t kAreaWidth = 32;
constexpr uint32_t kAreaHeight = 16;

//======================================================================================================================
lmx::rhi::Result<std::unique_ptr<lmx::rhi::GraphicsPipeline>>
makeRenderAreaPipeline(lmx::rhi::Device& device, lmx::rhi::ShaderLibrary& library) {
    return device.createGraphicsPipeline({.library = &library,
                                          .vertexEntry = "vertexMain",
                                          .fragmentEntry = "fragmentMain",
                                          .colorFormat = lmx::rhi::Format::BGRA8Unorm,
                                          .label = "lmx.test.renderAreaPipeline"});
}

//======================================================================================================================
// One fullscreen triangle into `target`, with the render area the caller asks for. The clear is
// blue and the fragment stage is red, so every readback below reads one or the other.
std::vector<uint8_t> drawWithRenderArea(lmx::rhi::Device& device,
                                        lmx::rhi::GraphicsPipeline& pipeline,
                                        lmx::rhi::Texture& target, uint32_t width,
                                        uint32_t height) {
    lmx::rhi::CommandList& commands = device.beginFrame();
    commands.beginRenderPass({.colorTarget = &target,
                              .clearColor = {0.0f, 0.0f, 1.0f, 1.0f},
                              .clear = true,
                              .renderAreaWidth = width,
                              .renderAreaHeight = height,
                              .label = "lmx.test.renderArea"});
    commands.bindPipeline(pipeline);
    commands.draw(3);
    commands.endRenderPass();
    device.endFrame(nullptr);
    device.waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    target.readback(pixels.data(), pixels.size());
    return pixels;
}

//======================================================================================================================
void requireRed(const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y) {
    const Pixel p = pixelAt(pixels, x, y);
    INFO(describe("inside the render area", x, y, p));
    REQUIRE(p.r == 255);
    REQUIRE(p.g == 0);
    REQUIRE(p.b == 0);
}

//======================================================================================================================
void requireCleared(const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y) {
    const Pixel p = pixelAt(pixels, x, y);
    INFO(describe("outside the render area", x, y, p));
    REQUIRE(p.r == 0);
    REQUIRE(p.g == 0);
    REQUIRE(p.b == 255);
}

} // namespace

//======================================================================================================================
// The whole point of the field: a pass draws into an origin-anchored corner of a target allocated
// at the full extent, and the texels outside that corner keep the pass's clear.
TEST_CASE("a render area confines a pass to an origin-anchored sub-rectangle", "[gpu][rhi]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto target = makeProbeTarget(**device, "lmx.test.renderAreaColor");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/RenderAreaSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = makeRenderAreaPipeline(**device, **library);
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    const std::vector<uint8_t> pixels =
        drawWithRenderArea(**device, **pipeline, **target, kAreaWidth, kAreaHeight);

    // The area's corners and middle, then the three regions around it: to its right, below it, and
    // diagonally past it.
    requireRed(pixels, 0, 0);
    requireRed(pixels, kAreaWidth - 1, kAreaHeight - 1);
    requireRed(pixels, kAreaWidth / 2, kAreaHeight / 2);
    requireCleared(pixels, kAreaWidth, 0);
    requireCleared(pixels, 0, kAreaHeight);
    requireCleared(pixels, kAreaWidth, kAreaHeight);
    requireCleared(pixels, kSize - 1, kSize - 1);
}

//======================================================================================================================
// The unset area is the pre-existing behaviour every other pass relies on: the same draw covers
// the whole attachment, with no scissor left over from the case above.
TEST_CASE("an unset render area covers the whole attachment", "[gpu][rhi]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto target = makeProbeTarget(**device, "lmx.test.fullAreaColor");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/RenderAreaSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = makeRenderAreaPipeline(**device, **library);
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    const std::vector<uint8_t> pixels = drawWithRenderArea(**device, **pipeline, **target, 0, 0);

    requireRed(pixels, 0, 0);
    requireRed(pixels, kAreaWidth, kAreaHeight);
    requireRed(pixels, kSize - 1, kSize - 1);
}

//======================================================================================================================
// An area past the attachment is caller misuse, which beginRenderPass reports through LMX_ASSERT
// and so cannot be provoked from a test. The condition it asserts on is the validate function, so
// that is what this checks -- here rather than in the CPU-only suite, because a depth-only pass's
// attachment is a real device texture.
TEST_CASE("a render area past a depth-only pass's attachment is rejected", "[gpu][rhi]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto depth = (*device)->createTexture({.width = kSize,
                                           .height = kSize,
                                           .format = Format::D32Float,
                                           .renderTarget = true,
                                           .label = "lmx.test.renderAreaDepth"});
    INFO(errorOf(depth));
    REQUIRE(depth.has_value());

    const auto r = validateRenderArea(nullptr, depth->get(), kSize, kSize + 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("64x65"));
}
