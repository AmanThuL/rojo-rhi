#include "RhiGpuTestSupport.h"

// The existing indirect cases used base instance zero. Nonzero base plus four local instances
// distinguishes the native full index from HLSL-style local index lowering.
//======================================================================================================================
TEST_CASE("indirect instance index includes first instance", "[gpu][visibility][instance-index]") {
    using namespace lmx::rhi;
    auto device = createDevice();
    REQUIRE(device.has_value());
    auto library = (*device)->loadShaderLibrary("Shaders/InstanceIndex");
    INFO(errorOf(library));
    REQUIRE(library.has_value());
    auto pipeline =
        (*device)->createGraphicsPipeline({.library = library->get(),
                                           .vertexEntry = "vertexMain",
                                           .fragmentEntry = "fragmentMain",
                                           .colorFormat = Format::BGRA8Unorm,
                                           .cullMode = CullMode::None,
                                           .label = "lmx.test.visibility.instanceIndex"});
    INFO(errorOf(pipeline));
    REQUIRE(pipeline.has_value());
    auto target = makeProbeTarget(**device, "lmx.test.visibility.indexTarget");
    REQUIRE(target.has_value());
    const uint32_t indices[] = {0, 1, 2};
    auto indexBuffer = (*device)->createBuffer(
        {.size = sizeof(indices), .label = "lmx.test.visibility.indices"}, indices);
    REQUIRE(indexBuffer.has_value());
    for (uint32_t count : {1u, 4u}) {
        constexpr uint32_t sentinel = 0xffffffffu;
        std::array<uint32_t, 16> initial;
        initial.fill(sentinel);
        auto output = (*device)->createBuffer({.size = sizeof(initial),
                                               .storageWrite = true,
                                               .cpuReadback = true,
                                               .label = "lmx.test.visibility.observed"},
                                              initial.data());
        REQUIRE(output.has_value());
        constexpr uint64_t offset = 32;
        std::array<uint8_t, 64> bytes{};
        DrawIndexedIndirectArgs args{.indexCount = 3, .instanceCount = count, .firstInstance = 5};
        std::memcpy(bytes.data() + offset, &args, sizeof(args));
        auto arguments = (*device)->createBuffer(
            {.size = bytes.size(), .label = "lmx.test.visibility.args"}, bytes.data());
        REQUIRE(arguments.has_value());
        auto& commands = (*device)->beginFrame();
        commands.beginRenderPass({.colorTarget = target->get(),
                                  .clear = true,
                                  .label = "lmx.test.visibility.indexPass"});
        commands.bindPipeline(**pipeline);
        commands.bindBuffer(0, **output);
        commands.drawIndexedIndirect(**indexBuffer, **arguments, offset);
        commands.endRenderPass();
        (*device)->endFrame(nullptr);
        (*device)->waitIdle();
        std::array<uint32_t, 16> actual{};
        (*output)->readback(actual.data(), sizeof(actual));
        for (uint32_t index = 0; index < actual.size(); ++index) {
            INFO("instance count " << count << " output " << index);
            REQUIRE(actual[index] == (index >= 5 && index < 5 + count ? index : sentinel));
        }
    }
}
