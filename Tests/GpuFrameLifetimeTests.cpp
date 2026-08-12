#include "GpuTestSupport.h"

namespace {

constexpr uint64_t kRingBytes = 256 * 1024;
constexpr uint64_t kRingAlignment = 256;

constexpr uint32_t kRingFillWrites = 700;
static_assert(kRingFillWrites * kRingAlignment * 10 > kRingBytes * 6,
              "the stress frame must fill more than 60% of the per-frame ring");
static_assert(kRingFillWrites * kRingAlignment < kRingBytes,
              "the stress frame must fit in the per-frame ring");

constexpr uint32_t kOverlapFrames = 12;

struct FrameColor {
    uint8_t r = 0, g = 0, b = 0;
};

//======================================================================================================================
FrameColor overlapFrameColor(uint32_t frame) {
    return {.r = static_cast<uint8_t>(20 * (frame + 1)),
            .g = static_cast<uint8_t>(250 - 20 * frame),
            .b = 60};
}

constexpr int kOverlapTolerance = 3;

} // namespace

//======================================================================================================================
// The number a caller joins its own per-frame state to. The case pins both halves of the contract:
// the count starts at zero and follows beginFrame, and it names the frame being built rather than
// the frame passTimingsFrame() reports, which is always an earlier one.
TEST_CASE("the device numbers the frames beginFrame opens", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    REQUIRE((*device)->frameNumber() == 0);

    for (uint64_t frame = 1; frame <= 3; ++frame) {
        CommandList& commands = (*device)->beginFrame();
        INFO("frame " + std::to_string(frame));
        REQUIRE((*device)->frameNumber() == frame);
        commands.beginComputePass("lmx.test.frameLifetime.empty");
        commands.endComputePass();
        // An open frame's number is fixed: everything recorded here belongs to that frame.
        REQUIRE((*device)->frameNumber() == frame);
        (*device)->endFrame(nullptr);
        REQUIRE((*device)->frameNumber() == frame);
    }

    // The one sequence passTimings() documents as publishing a specific frame: end it, drain, then
    // open one more frame. The measured frame is the drained one, three behind the open one.
    (*device)->waitIdle();
    (*device)->beginFrame();
    REQUIRE((*device)->frameNumber() == 4);
    REQUIRE((*device)->passTimingsFrame() == 3);
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();
}

//======================================================================================================================
// Twelve undrained frames recycle each of three ring slots three times.
// Each frame fills 68% of its slot with a distinct color and renders to its own target, so a stomp
// cannot be hidden by later draws. The only waitIdle occurs after all submissions.
TEST_CASE("uniform ring survives twelve frames overlapping in flight", "[gpu][checkpoint-a]") {
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
                                                       .label = "lmx.test.overlapPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    std::vector<std::unique_ptr<Texture>> targets;
    targets.reserve(kOverlapFrames);
    for (uint32_t frame = 0; frame < kOverlapFrames; ++frame) {
        const std::string label = "lmx.test.overlapTarget." + std::to_string(frame);
        auto target = (*device)->createTexture({.width = kSize,
                                                .height = kSize,
                                                .format = Format::BGRA8Unorm,
                                                .renderTarget = true,
                                                .cpuReadback = true,
                                                .label = label});
        INFO(errorOf(target));
        REQUIRE(target.has_value());
        targets.push_back(std::move(*target));
    }

    for (uint32_t frame = 0; frame < kOverlapFrames; ++frame) {
        const FrameColor color = overlapFrameColor(frame);

        std::array<Vertex, 3> vertices = kTriangle;
        for (Vertex& vertex : vertices) {
            vertex.color[0] = static_cast<float>(color.r) / 255.0f;
            vertex.color[1] = static_cast<float>(color.g) / 255.0f;
            vertex.color[2] = static_cast<float>(color.b) / 255.0f;
        }

        CommandList& commands = (*device)->beginFrame();
        commands.beginRenderPass({.colorTarget = targets[frame].get(),
                                  .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                                  .clear = true,
                                  .label = "lmx.test.frameLifetime.overlap"});
        commands.bindPipeline(**pipeline);

        for (uint32_t write = 0; write < kRingFillWrites; ++write) {
            commands.bindFrameData(kVertexBufferSlot, vertices.data(), sizeof(vertices));
        }

        commands.draw(static_cast<uint32_t>(vertices.size()));
        commands.endRenderPass();
        (*device)->endFrame(nullptr);
    }

    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    for (uint32_t frame = 0; frame < kOverlapFrames; ++frame) {
        targets[frame]->readback(pixels.data(), pixels.size());

        const Pixel inside = pixelAt(pixels, 32, 40);
        const FrameColor color = overlapFrameColor(frame);
        INFO("frame " + std::to_string(frame) + " expected R=" + std::to_string(color.r) +
             " G=" + std::to_string(color.g) + " B=" + std::to_string(color.b) +
             " -- a wrong R names the frame whose uniforms were actually read (R = 20*(N+1))");
        INFO(describe("inside", 32, 40, inside));
        REQUIRE(channelNear(inside.r, color.r, kOverlapTolerance));
        REQUIRE(channelNear(inside.g, color.g, kOverlapTolerance));
        REQUIRE(channelNear(inside.b, color.b, kOverlapTolerance));
        REQUIRE(inside.a == 255);

        const Pixel corner = pixelAt(pixels, 2, 2);
        INFO(describe("corner", 2, 2, corner));
        REQUIRE(corner.r == 0);
        REQUIRE(corner.g == 0);
        REQUIRE(corner.b == 0);
        REQUIRE(corner.a == 255);
    }
}
