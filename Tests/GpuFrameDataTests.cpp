#include "GpuTestSupport.h"

#include "RHI/Metal4/Metal4FrameData.h"

namespace {

using lmx::rhi::metal4::FrameDataCounters;
using lmx::rhi::metal4::frameDataCounters;
using lmx::rhi::metal4::kFrameDataPageBytes;
using lmx::rhi::metal4::kFrameDataSlotCount;

using Triangle = std::array<Vertex, 3>;

// Effective stride of one default-aligned block, which is what makes the fill counts below exact
// rather than approximate: a 60-byte triangle advances the cursor to the next 256-byte boundary.
constexpr uint64_t kBlockStride = lmx::rhi::kFrameDataAlignment;
static_assert(sizeof(Triangle) <= kBlockStride, "a triangle block must fit inside one stride");

constexpr uint32_t kBlocksPerPage = static_cast<uint32_t>(kFrameDataPageBytes / kBlockStride);

// Enough blocks to leave the first page behind with room to spare in the second.
constexpr uint32_t kSpillWrites = kBlocksPerPage + 32;

constexpr uint32_t kOverlapFrames = 12;

//======================================================================================================================
// Two disjoint triangles, each safely interior at its probe. The fill triangle covers the whole
// target in a colour neither probe expects, so a draw that read a filler block instead of the block
// bound right before it fails both probes rather than passing by luck.
Triangle leftTriangle(float r, float g, float b) {
    return {{{{-0.9f, -0.9f}, {r, g, b}}, {{-0.1f, -0.9f}, {r, g, b}}, {{-0.5f, 0.9f}, {r, g, b}}}};
}

//======================================================================================================================
Triangle rightTriangle(float r, float g, float b) {
    return {{{{0.1f, -0.9f}, {r, g, b}}, {{0.9f, -0.9f}, {r, g, b}}, {{0.5f, 0.9f}, {r, g, b}}}};
}

//======================================================================================================================
Triangle fillTriangle() {
    return {{{{-3.0f, -1.0f}, {1.0f, 1.0f, 1.0f}},
             {{3.0f, -1.0f}, {1.0f, 1.0f, 1.0f}},
             {{0.0f, 3.0f}, {1.0f, 1.0f, 1.0f}}}};
}

//======================================================================================================================
uint32_t slotOf(const lmx::rhi::Device& device) {
    return static_cast<uint32_t>(device.frameNumber() % kFrameDataSlotCount);
}

//======================================================================================================================
lmx::rhi::Result<std::unique_ptr<lmx::rhi::GraphicsPipeline>>
makeTrianglePipeline(lmx::rhi::Device& device, lmx::rhi::ShaderLibrary& library,
                     const char* label) {
    return device.createGraphicsPipeline({.library = &library,
                                          .vertexEntry = "vertexMain",
                                          .fragmentEntry = "fragmentMain",
                                          .colorFormat = lmx::rhi::Format::BGRA8Unorm,
                                          .label = label});
}

} // namespace

//======================================================================================================================
// The whole contract of one call, end to end: the block is copied, its address is bound to the slot
// the draw reads, and the address handed back is a real one placed at the default alignment.
TEST_CASE("bindFrameData binds a block the next draw reads", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = makeTrianglePipeline(**device, **library, "lmx.test.frameData.pipeline");
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto target = makeProbeTarget(**device, "lmx.test.frameData.target");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    const Triangle left = leftTriangle(1.0f, 0.0f, 0.0f);
    const Triangle right = rightTriangle(0.0f, 0.0f, 1.0f);

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.frameData.basic"});
    commands.bindPipeline(**pipeline);

    const GpuAddress leftAddress = commands.bindFrameData(kVertexBufferSlot, left);
    commands.draw(3);
    const GpuAddress rightAddress = commands.bindFrameData(kVertexBufferSlot, right);
    commands.draw(3);

    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    REQUIRE(leftAddress.isValid());
    REQUIRE(rightAddress.isValid());
    REQUIRE_FALSE(leftAddress == rightAddress);
    REQUIRE(leftAddress.value % kFrameDataAlignment == 0);
    REQUIRE(rightAddress.value % kFrameDataAlignment == 0);
    // A copy of exactly the bytes asked for, laid out end to end at the default alignment.
    REQUIRE(rightAddress.value - leftAddress.value == kBlockStride);

    const FrameDataCounters counters = frameDataCounters(**device);
    REQUIRE(counters.calls == 2);
    REQUIRE(counters.bytes == 2 * sizeof(Triangle));
    REQUIRE(counters.addressBinds == 2);

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*target)->readback(pixels.data(), pixels.size());

    const Pixel leftProbe = pixelAt(pixels, 16, 32);
    INFO(describe("left", 16, 32, leftProbe));
    REQUIRE(leftProbe.r == 255);
    REQUIRE(leftProbe.g == 0);
    REQUIRE(leftProbe.b == 0);

    const Pixel rightProbe = pixelAt(pixels, 48, 32);
    INFO(describe("right", 48, 32, rightProbe));
    REQUIRE(rightProbe.r == 0);
    REQUIRE(rightProbe.g == 0);
    REQUIRE(rightProbe.b == 255);
}

//======================================================================================================================
// Growth inside one frame. The first draw reads a block from the slot's original page; enough fill
// blocks to exhaust that page follow; the second draw reads a block the backend had to add a page
// for. Both probes must still be exact, which is the part a page-relative address bug breaks.
TEST_CASE("bindFrameData spans pages within one frame", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = makeTrianglePipeline(**device, **library, "lmx.test.frameData.spillPipeline");
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto target = makeProbeTarget(**device, "lmx.test.frameData.spillTarget");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    const Triangle left = leftTriangle(0.0f, 1.0f, 0.0f);
    const Triangle right = rightTriangle(1.0f, 0.0f, 1.0f);
    const Triangle filler = fillTriangle();

    CommandList& commands = (*device)->beginFrame();
    const uint32_t slot = slotOf(**device);

    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.frameData.spill"});
    commands.bindPipeline(**pipeline);

    const GpuAddress leftAddress = commands.bindFrameData(kVertexBufferSlot, left);
    commands.draw(3);
    for (uint32_t write = 0; write < kSpillWrites; ++write) {
        commands.bindFrameData(kVertexBufferSlot, filler);
    }
    const GpuAddress rightAddress = commands.bindFrameData(kVertexBufferSlot, right);
    commands.draw(3);

    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    const FrameDataCounters counters = frameDataCounters(**device);
    INFO("slot " + std::to_string(slot) + " used " +
         std::to_string(counters.slots[slot].pagesUsed) + " page(s) of " +
         std::to_string(counters.slots[slot].pageCount));
    REQUIRE(counters.slots[slot].pageCount >= 2);
    REQUIRE(counters.slots[slot].pagesUsed >= 2);
    // Growth is by pages, so one extra page covers the spill however many blocks caused it.
    REQUIRE(counters.pageCreations == kFrameDataSlotCount + 1);
    REQUIRE(counters.calls == kSpillWrites + 2);
    REQUIRE(leftAddress.isValid());
    REQUIRE(rightAddress.isValid());

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*target)->readback(pixels.data(), pixels.size());

    const Pixel leftProbe = pixelAt(pixels, 16, 32);
    INFO(describe("left", 16, 32, leftProbe));
    REQUIRE(leftProbe.r == 0);
    REQUIRE(leftProbe.g == 255);
    REQUIRE(leftProbe.b == 0);

    const Pixel rightProbe = pixelAt(pixels, 48, 32);
    INFO(describe("right", 48, 32, rightProbe));
    REQUIRE(rightProbe.r == 255);
    REQUIRE(rightProbe.g == 0);
    REQUIRE(rightProbe.b == 255);
}

//======================================================================================================================
// A request larger than a normal page gets a page of its own, sized to the request rounded up to
// the page quantum -- not a buffer per call and not an oversized default page for every slot.
TEST_CASE("an oversized frame-data block gets a page rounded to the quantum", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    // Between one and two normal pages, so the rounded capacity is unambiguously two quanta.
    const uint64_t oversized = kFrameDataPageBytes + kFrameDataPageBytes / 2;
    const std::vector<uint8_t> block(oversized, 0x5a);

    CommandList& commands = (*device)->beginFrame();
    const uint32_t slot = slotOf(**device);
    commands.beginComputePass("lmx.test.frameData.oversized");
    const GpuAddress address = commands.bindFrameData(1, block.data(), block.size());
    commands.endComputePass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    REQUIRE(address.isValid());
    REQUIRE(address.value % kFrameDataAlignment == 0);

    const FrameDataCounters counters = frameDataCounters(**device);
    REQUIRE(counters.slots[slot].pageCount == 2);
    REQUIRE(counters.slots[slot].capacityBytes == kFrameDataPageBytes + 2 * kFrameDataPageBytes);
    REQUIRE(counters.slots[slot].bytesUsed == oversized);
    // Only the slot that saw the request grew; the other two still own their original page.
    REQUIRE(counters.pageCreations == kFrameDataSlotCount + 1);
}

//======================================================================================================================
// A block exactly one normal page large can still need leading padding when its requested
// alignment is wider than the page quantum. The grown page reserves that worst case rather than
// passing validation and then asserting merely because Metal chose a less-aligned GPU base.
TEST_CASE("an oversized frame-data page reserves wide-alignment padding", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    constexpr uint64_t kWideAlignment = 2 * kFrameDataPageBytes;
    const std::vector<uint8_t> block(kFrameDataPageBytes, 0x3c);

    CommandList& commands = (*device)->beginFrame();
    const uint32_t slot = slotOf(**device);
    commands.beginComputePass("lmx.test.frameData.wideOversized");
    const GpuAddress address =
        commands.bindFrameData(1, block.data(), block.size(), kWideAlignment);
    commands.endComputePass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    REQUIRE(address.isValid());
    REQUIRE(address.value % kWideAlignment == 0);
    const FrameDataCounters counters = frameDataCounters(**device);
    REQUIRE(counters.slots[slot].pageCount == 2);
    REQUIRE(counters.slots[slot].capacityBytes == 4 * kFrameDataPageBytes);
}

//======================================================================================================================
// A wider alignment than the default is honoured on the returned address, and does not disturb the
// placement of the ordinary blocks around it.
TEST_CASE("bindFrameData honours an alignment wider than the default", "[gpu]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    constexpr uint64_t kWide = 4096;
    const std::array<uint32_t, 4> block{1, 2, 3, 4};

    CommandList& commands = (*device)->beginFrame();
    commands.beginComputePass("lmx.test.frameData.alignment");
    const GpuAddress first = commands.bindFrameData(0, block.data(), sizeof(block));
    const GpuAddress wide = commands.bindFrameData(1, block.data(), sizeof(block), kWide);
    const GpuAddress after = commands.bindFrameData(2, block.data(), sizeof(block));
    commands.endComputePass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    REQUIRE(first.value % kFrameDataAlignment == 0);
    REQUIRE(wide.value % kWide == 0);
    REQUIRE(wide.value > first.value);
    // The padding the wide block needed is skipped, not copied, and the next block resumes right
    // after it at the default alignment.
    REQUIRE(after.value % kFrameDataAlignment == 0);
    REQUIRE(after.value - wide.value == kFrameDataAlignment);
}

//======================================================================================================================
// Twelve undrained frames recycle each of three slots four times. Every frame spills into a second
// page and renders its own colour into its own target, so a slot reset before the GPU retired its
// previous owner shows up as one frame reading another's blocks. The only waitIdle is after every
// submission, and the arena's own recycle assert fires first if the pacing proof were skipped.
TEST_CASE("the frame-data arena survives twelve frames overlapping in flight",
          "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = makeTrianglePipeline(**device, **library, "lmx.test.frameData.overlapPipeline");
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    std::vector<std::unique_ptr<Texture>> targets;
    targets.reserve(kOverlapFrames);
    for (uint32_t frame = 0; frame < kOverlapFrames; ++frame) {
        auto target = makeProbeTarget(
            **device, ("lmx.test.frameData.overlap." + std::to_string(frame)).c_str());
        INFO(errorOf(target));
        REQUIRE(target.has_value());
        targets.push_back(std::move(*target));
    }

    const Triangle filler = fillTriangle();
    uint32_t highWaterCreations = 0;
    std::array<uint64_t, kFrameDataSlotCount> slotBytes{};

    for (uint32_t frame = 0; frame < kOverlapFrames; ++frame) {
        const float green = static_cast<float>(20 * (frame + 1)) / 255.0f;
        const Triangle left = leftTriangle(1.0f, green, 0.0f);

        CommandList& commands = (*device)->beginFrame();
        const uint32_t slot = slotOf(**device);

        commands.beginRenderPass({.colorTarget = targets[frame].get(),
                                  .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                                  .clear = true,
                                  .label = "lmx.test.frameData.overlap"});
        commands.bindPipeline(**pipeline);
        for (uint32_t write = 0; write < kSpillWrites; ++write) {
            commands.bindFrameData(kVertexBufferSlot, filler);
        }
        commands.bindFrameData(kVertexBufferSlot, left);
        commands.draw(3);
        commands.endRenderPass();
        (*device)->endFrame(nullptr);

        const FrameDataCounters counters = frameDataCounters(**device);
        // Every frame does identical work, so a slot's second visit must consume exactly what its
        // first did: an inexact cursor reset shows up here as drift rather than as a wrong pixel.
        if (frame < kFrameDataSlotCount) {
            slotBytes[slot] = counters.slots[slot].bytesUsed;
        } else {
            INFO("frame " + std::to_string(frame) + " on slot " + std::to_string(slot));
            REQUIRE(counters.slots[slot].bytesUsed == slotBytes[slot]);
        }
        // Once all three slots have seen the workload, its pages are retained and reused.
        if (frame + 1 == kFrameDataSlotCount) {
            highWaterCreations = counters.pageCreations;
        } else if (frame + 1 > kFrameDataSlotCount) {
            REQUIRE(counters.pageCreations == highWaterCreations);
        }
    }

    (*device)->waitIdle();

    const FrameDataCounters counters = frameDataCounters(**device);
    REQUIRE(counters.calls == uint64_t{kOverlapFrames} * (kSpillWrites + 1));
    // Two pages per slot and nothing more: growth is bounded by retained high water per slot, not
    // by frame count.
    REQUIRE(counters.pageCreations == 2 * kFrameDataSlotCount);

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    for (uint32_t frame = 0; frame < kOverlapFrames; ++frame) {
        targets[frame]->readback(pixels.data(), pixels.size());

        const Pixel probe = pixelAt(pixels, 16, 32);
        const int expectedGreen = static_cast<int>(20 * (frame + 1));
        INFO("frame " + std::to_string(frame) + " expected G=" + std::to_string(expectedGreen) +
             " -- a wrong G names the frame whose blocks were actually read (G = 20*(N+1))");
        INFO(describe("left", 16, 32, probe));
        REQUIRE(probe.r == 255);
        REQUIRE(channelNear(probe.g, expectedGreen, 3));
        REQUIRE(probe.b == 0);

        const Pixel corner = pixelAt(pixels, 2, 2);
        INFO(describe("corner", 2, 2, corner));
        REQUIRE(corner.r == 0);
        REQUIRE(corner.g == 0);
        REQUIRE(corner.b == 0);
    }
}
