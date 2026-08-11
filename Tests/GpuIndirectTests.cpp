#include "GpuTestSupport.h"

namespace {

// Mirrors ComputeSmoke.slang's ComputeParams; the layout is pinned by the shader, not chosen here.
struct IndirectComputeParams {
    uint32_t bias = 0;
    uint32_t extent = 0;
};

// Mirrors IndirectSmoke.slang's IndirectParams.
struct IndirectParams {
    uint32_t argsIndex = 0;
    uint32_t threadgroupsX = 0;
    uint32_t indexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t baseVertex = 0;
};

// computeFillBuffer's [numthreads], which is what turns a threadgroup count into an element count.
constexpr uint32_t kIndirectThreadsPerGroup = 64;

// The element count the arguments ask for, and the buffer that holds twice as many: the elements
// past the dispatched range must keep their sentinel, or a dispatch that ignored the count in the
// buffer and covered the whole allocation would pass.
constexpr uint32_t kDispatchedGroups = 4;
constexpr uint32_t kDispatchedElements = kDispatchedGroups * kIndirectThreadsPerGroup;
constexpr uint32_t kOutputElements = kDispatchedElements * 2;
constexpr uint32_t kSentinel = 0xFFFFFFFFu;
constexpr uint32_t kIndirectBias = 5;

// Three degenerate off-screen vertices followed by the real triangle. A draw that dropped its
// first-vertex or base-vertex argument pulls the degenerate ones and rasterizes nothing, so the
// probes below distinguish "the offset was honored" from "something was drawn".
constexpr std::array<Vertex, 6> kOffsetTriangle = {{
    {{-4.0f, -4.0f}, {1.0f, 1.0f, 1.0f}},
    {{-4.0f, -4.0f}, {1.0f, 1.0f, 1.0f}},
    {{-4.0f, -4.0f}, {1.0f, 1.0f, 1.0f}},
    {{0.0f, 0.5f}, {1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
}};

// The triangle's indices sit past three ignorable ones, so firstIndex has to be honored too.
constexpr std::array<uint32_t, 6> kOffsetIndices = {{0, 0, 0, 0, 1, 2}};
constexpr uint32_t kFirstIndex = 3;
constexpr uint32_t kBaseVertex = 3;

//======================================================================================================================
// Every element the dispatch covered carries computeFillBuffer's arithmetic; every element past it
// still carries the sentinel the buffer was created with.
void requireDispatchedRange(const std::vector<uint32_t>& values) {
    for (uint32_t index = 0; index < kOutputElements; ++index) {
        INFO("element " + std::to_string(index));
        const uint32_t expected =
            index < kDispatchedElements ? index * 3 + kIndirectBias : kSentinel;
        REQUIRE(values[index] == expected);
    }
}

//======================================================================================================================
// The triangle covers the middle of the target and no corner of it, so a black clear makes "drawn"
// and "not drawn" separable on every channel.
void requireTriangleDrawn(const std::vector<uint8_t>& pixels) {
    const Pixel center = pixelAt(pixels, kSize / 2, kSize / 2);
    INFO(describe("center", kSize / 2, kSize / 2, center));
    REQUIRE(center.r > 0);
    REQUIRE(center.g > 0);
    REQUIRE(center.b > 0);

    const Pixel corner = pixelAt(pixels, 1, 1);
    INFO(describe("corner", 1, 1, corner));
    REQUIRE(corner.r == 0);
    REQUIRE(corner.g == 0);
    REQUIRE(corner.b == 0);
}

//======================================================================================================================
// The vertex-pulling triangle pipeline every draw case below records into.
lmx::rhi::Result<std::unique_ptr<lmx::rhi::GraphicsPipeline>>
makeTrianglePipeline(lmx::rhi::Device& device, lmx::rhi::ShaderLibrary& library,
                     const char* label) {
    return device.createGraphicsPipeline({.library = &library,
                                          .vertexEntry = "vertexMain",
                                          .fragmentEntry = "fragmentMain",
                                          .colorFormat = lmx::rhi::Format::BGRA8Unorm,
                                          .cullMode = lmx::rhi::CullMode::None,
                                          .label = label});
}

} // namespace

//======================================================================================================================
// The threadgroup count comes from a struct the CPU wrote at a non-zero offset, so the case pins
// both the argument layout and the offset arithmetic: reading the buffer from byte zero would find
// the padding pattern rather than the counts.
TEST_CASE("an indirect dispatch reads its threadgroup counts from a buffer",
          "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint64_t kArgsOffset = 64;
    constexpr uint64_t kArgsBytes = 128;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/ComputeSmoke");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline =
        (*device)->createComputePipeline({.library = library->get(),
                                          .computeEntry = "computeFillBuffer",
                                          .threadsPerThreadgroup = {kIndirectThreadsPerGroup, 1, 1},
                                          .label = "lmx.test.indirect.dispatchPipeline"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    // Everything outside the arguments is a pattern no valid count could be mistaken for.
    std::vector<uint8_t> argsBytes(kArgsBytes, 0xCD);
    const DispatchIndirectArgs args{
        .threadgroupsX = kDispatchedGroups, .threadgroupsY = 1, .threadgroupsZ = 1};
    std::memcpy(argsBytes.data() + kArgsOffset, &args, sizeof(args));
    auto argumentBuffer = (*device)->createBuffer(
        {.size = kArgsBytes, .label = "lmx.test.indirect.dispatchArgs"}, argsBytes.data());
    INFO(errorOf(argumentBuffer));
    REQUIRE(argumentBuffer.has_value());

    const std::vector<uint32_t> sentinel(kOutputElements, kSentinel);
    auto output = (*device)->createBuffer({.size = sizeof(uint32_t) * kOutputElements,
                                           .storageWrite = true,
                                           .cpuReadback = true,
                                           .label = "lmx.test.indirect.dispatchOutput"},
                                          sentinel.data());
    INFO(errorOf(output));
    REQUIRE(output.has_value());

    const IndirectComputeParams params{.bias = kIndirectBias, .extent = 0};

    CommandList& commands = (*device)->beginFrame();
    commands.beginComputePass("lmx.test.indirect.dispatch");
    commands.bindComputePipeline(**pipeline);
    commands.bindStorageBuffer(0, **output, StorageAccess::Write);
    commands.setUniforms(1, &params, sizeof(params));
    commands.dispatchIndirect(**argumentBuffer, kArgsOffset);
    commands.endComputePass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint32_t> values(kOutputElements, 0);
    (*output)->readback(values.data(), values.size() * sizeof(uint32_t));
    requireDispatchedRange(values);
}

//======================================================================================================================
// The non-indexed draw, whose firstVertex has to reach the vertex-pulling shader for anything to
// appear at all.
TEST_CASE("an indirect draw reads its vertex range from a buffer", "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint64_t kArgsOffset = 32;
    constexpr uint64_t kArgsBytes = 128;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline = makeTrianglePipeline(**device, **library, "lmx.test.indirect.drawPipeline");
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto vertices = (*device)->createBuffer(
        {.size = sizeof(kOffsetTriangle), .label = "lmx.test.indirect.drawVertices"},
        kOffsetTriangle.data());
    INFO(errorOf(vertices));
    REQUIRE(vertices.has_value());

    std::vector<uint8_t> argsBytes(kArgsBytes, 0xCD);
    const DrawIndirectArgs args{
        .vertexCount = 3, .instanceCount = 1, .firstVertex = 3, .firstInstance = 0};
    std::memcpy(argsBytes.data() + kArgsOffset, &args, sizeof(args));
    auto argumentBuffer = (*device)->createBuffer(
        {.size = kArgsBytes, .label = "lmx.test.indirect.drawArgs"}, argsBytes.data());
    INFO(errorOf(argumentBuffer));
    REQUIRE(argumentBuffer.has_value());

    auto target = makeProbeTarget(**device, "lmx.test.indirect.drawTarget");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.indirect.draw"});
    commands.bindPipeline(**pipeline);
    commands.bindBuffer(kVertexBufferSlot, **vertices);
    commands.drawIndirect(**argumentBuffer, kArgsOffset);
    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*target)->readback(pixels.data(), pixels.size());
    requireTriangleDrawn(pixels);
}

//======================================================================================================================
// The indexed variant, with a non-zero firstIndex and a non-zero baseVertex: both have to be read
// out of the struct at the documented offsets, and either being dropped leaves the target black.
TEST_CASE("an indirect indexed draw reads its index range and base vertex from a buffer",
          "[gpu][checkpoint-a]") {
    using namespace lmx::rhi;

    constexpr uint64_t kArgsOffset = 48;
    constexpr uint64_t kArgsBytes = 128;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto library = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(library));
    REQUIRE(library.has_value());

    auto pipeline =
        makeTrianglePipeline(**device, **library, "lmx.test.indirect.drawIndexedPipeline");
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto vertices = (*device)->createBuffer(
        {.size = sizeof(kOffsetTriangle), .label = "lmx.test.indirect.indexedVertices"},
        kOffsetTriangle.data());
    INFO(errorOf(vertices));
    REQUIRE(vertices.has_value());

    auto indices = (*device)->createBuffer(
        {.size = sizeof(kOffsetIndices), .label = "lmx.test.indirect.indexedIndices"},
        kOffsetIndices.data());
    INFO(errorOf(indices));
    REQUIRE(indices.has_value());

    std::vector<uint8_t> argsBytes(kArgsBytes, 0xCD);
    const DrawIndexedIndirectArgs args{.indexCount = 3,
                                       .instanceCount = 1,
                                       .firstIndex = kFirstIndex,
                                       .baseVertex = static_cast<int32_t>(kBaseVertex),
                                       .firstInstance = 0};
    std::memcpy(argsBytes.data() + kArgsOffset, &args, sizeof(args));
    auto argumentBuffer = (*device)->createBuffer(
        {.size = kArgsBytes, .label = "lmx.test.indirect.indexedArgs"}, argsBytes.data());
    INFO(errorOf(argumentBuffer));
    REQUIRE(argumentBuffer.has_value());

    auto target = makeProbeTarget(**device, "lmx.test.indirect.indexedTarget");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.indirect.drawIndexed"});
    commands.bindPipeline(**pipeline);
    commands.bindBuffer(kVertexBufferSlot, **vertices);
    commands.drawIndexedIndirect(**indices, **argumentBuffer, kArgsOffset);
    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*target)->readback(pixels.data(), pixels.size());
    requireTriangleDrawn(pixels);
}

//======================================================================================================================
// The arguments are written by a compute pass instead of by the CPU, ordered by a buffer barrier.
// The writing kernel lays down raw 32-bit words at the documented indices, so this is also what
// pins DispatchIndirectArgs' member order against what the GPU actually consumes.
TEST_CASE("an indirect dispatch consumes arguments a compute pass wrote", "[gpu]") {
    using namespace lmx::rhi;

    // Element index in the arguments buffer, and the byte offset it corresponds to.
    constexpr uint32_t kArgsIndex = 16;
    constexpr uint64_t kArgsOffset = kArgsIndex * sizeof(uint32_t);
    constexpr uint64_t kArgsBytes = 128;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto indirectLibrary = (*device)->loadShaderLibrary("Shaders/IndirectSmoke");
    INFO(errorOf(indirectLibrary));
    REQUIRE(indirectLibrary.has_value());

    auto writeArgsPipeline =
        (*device)->createComputePipeline({.library = indirectLibrary->get(),
                                          .computeEntry = "computeWriteDispatchArgs",
                                          .threadsPerThreadgroup = {1, 1, 1},
                                          .label = "lmx.test.indirect.writeDispatchArgsPipeline"});
    INFO(errorOf(writeArgsPipeline));
    REQUIRE(writeArgsPipeline.has_value());

    auto fillLibrary = (*device)->loadShaderLibrary("Shaders/ComputeSmoke");
    INFO(errorOf(fillLibrary));
    REQUIRE(fillLibrary.has_value());

    auto fillPipeline =
        (*device)->createComputePipeline({.library = fillLibrary->get(),
                                          .computeEntry = "computeFillBuffer",
                                          .threadsPerThreadgroup = {kIndirectThreadsPerGroup, 1, 1},
                                          .label = "lmx.test.indirect.gpuArgsFillPipeline"});
    INFO(errorOf(fillPipeline));
    REQUIRE(fillPipeline.has_value());

    // No initial contents: everything the dispatch reads has to come from the pass that writes it.
    auto argumentBuffer = (*device)->createBuffer(
        {.size = kArgsBytes, .storageWrite = true, .label = "lmx.test.indirect.gpuDispatchArgs"},
        nullptr);
    INFO(errorOf(argumentBuffer));
    REQUIRE(argumentBuffer.has_value());

    const std::vector<uint32_t> sentinel(kOutputElements, kSentinel);
    auto output = (*device)->createBuffer({.size = sizeof(uint32_t) * kOutputElements,
                                           .storageWrite = true,
                                           .cpuReadback = true,
                                           .label = "lmx.test.indirect.gpuDispatchOutput"},
                                          sentinel.data());
    INFO(errorOf(output));
    REQUIRE(output.has_value());

    const IndirectParams argsParams{.argsIndex = kArgsIndex, .threadgroupsX = kDispatchedGroups};
    const IndirectComputeParams fillParams{.bias = kIndirectBias, .extent = 0};

    CommandList& commands = (*device)->beginFrame();
    commands.beginComputePass("lmx.test.indirect.writeDispatchArgs");
    commands.bindComputePipeline(**writeArgsPipeline);
    commands.bindStorageBuffer(0, **argumentBuffer, StorageAccess::Write);
    commands.setUniforms(1, &argsParams, sizeof(argsParams));
    commands.dispatch(1, 1, 1);
    commands.endComputePass();

    commands.bufferBarrier(**argumentBuffer, BufferUse::StorageWrite, BufferUse::IndirectArgument);

    commands.beginComputePass("lmx.test.indirect.gpuDispatch");
    commands.bindComputePipeline(**fillPipeline);
    commands.bindStorageBuffer(0, **output, StorageAccess::Write);
    commands.setUniforms(1, &fillParams, sizeof(fillParams));
    commands.dispatchIndirect(**argumentBuffer, kArgsOffset);
    commands.endComputePass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint32_t> values(kOutputElements, 0);
    (*output)->readback(values.data(), values.size() * sizeof(uint32_t));
    requireDispatchedRange(values);
}

//======================================================================================================================
// The same across the compute-to-render boundary, and for the five-word struct: a compute pass
// writes the indexed-draw arguments, a barrier orders them, and the render pass draws what they
// say. All five words have to land where DrawIndexedIndirectArgs documents them, or the triangle
// is drawn from the degenerate vertices and the target stays black.
TEST_CASE("an indirect indexed draw consumes arguments a compute pass wrote", "[gpu]") {
    using namespace lmx::rhi;

    constexpr uint32_t kArgsIndex = 8;
    constexpr uint64_t kArgsOffset = kArgsIndex * sizeof(uint32_t);
    constexpr uint64_t kArgsBytes = 128;

    auto device = createDevice();
    INFO(errorOf(device));
    REQUIRE(device.has_value());

    auto indirectLibrary = (*device)->loadShaderLibrary("Shaders/IndirectSmoke");
    INFO(errorOf(indirectLibrary));
    REQUIRE(indirectLibrary.has_value());

    auto writeArgsPipeline =
        (*device)->createComputePipeline({.library = indirectLibrary->get(),
                                          .computeEntry = "computeWriteDrawIndexedArgs",
                                          .threadsPerThreadgroup = {1, 1, 1},
                                          .label = "lmx.test.indirect.writeIndexedArgsPipeline"});
    INFO(errorOf(writeArgsPipeline));
    REQUIRE(writeArgsPipeline.has_value());

    auto triangleLibrary = (*device)->loadShaderLibrary("Shaders/Triangle");
    INFO(errorOf(triangleLibrary));
    REQUIRE(triangleLibrary.has_value());

    auto pipeline =
        makeTrianglePipeline(**device, **triangleLibrary, "lmx.test.indirect.gpuIndexedPipeline");
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());

    auto vertices = (*device)->createBuffer(
        {.size = sizeof(kOffsetTriangle), .label = "lmx.test.indirect.gpuIndexedVertices"},
        kOffsetTriangle.data());
    INFO(errorOf(vertices));
    REQUIRE(vertices.has_value());

    auto indices = (*device)->createBuffer(
        {.size = sizeof(kOffsetIndices), .label = "lmx.test.indirect.gpuIndexedIndices"},
        kOffsetIndices.data());
    INFO(errorOf(indices));
    REQUIRE(indices.has_value());

    auto argumentBuffer = (*device)->createBuffer(
        {.size = kArgsBytes, .storageWrite = true, .label = "lmx.test.indirect.gpuIndexedArgs"},
        nullptr);
    INFO(errorOf(argumentBuffer));
    REQUIRE(argumentBuffer.has_value());

    auto target = makeProbeTarget(**device, "lmx.test.indirect.gpuIndexedTarget");
    INFO(errorOf(target));
    REQUIRE(target.has_value());

    const IndirectParams argsParams{.argsIndex = kArgsIndex,
                                    .indexCount = 3,
                                    .firstIndex = kFirstIndex,
                                    .baseVertex = kBaseVertex};

    CommandList& commands = (*device)->beginFrame();
    commands.beginComputePass("lmx.test.indirect.writeIndexedArgs");
    commands.bindComputePipeline(**writeArgsPipeline);
    commands.bindStorageBuffer(0, **argumentBuffer, StorageAccess::Write);
    commands.setUniforms(1, &argsParams, sizeof(argsParams));
    commands.dispatch(1, 1, 1);
    commands.endComputePass();

    commands.bufferBarrier(**argumentBuffer, BufferUse::StorageWrite, BufferUse::IndirectArgument);

    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.indirect.gpuIndexedDraw"});
    commands.bindPipeline(**pipeline);
    commands.bindBuffer(kVertexBufferSlot, **vertices);
    commands.drawIndexedIndirect(**indices, **argumentBuffer, kArgsOffset);
    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    (*target)->readback(pixels.data(), pixels.size());
    requireTriangleDrawn(pixels);
}
