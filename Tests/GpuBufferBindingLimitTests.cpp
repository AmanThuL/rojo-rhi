#include "RhiGpuTestSupport.h"

//======================================================================================================================
TEST_CASE("compute reads the highest public buffer slot", "[gpu][rhi]") {
    using namespace rojoRHI;
    auto device = createDevice();
    REQUIRE(device);
    const uint32_t expected = 0x71cba513;
    auto input =
        (*device)->createBuffer({.size = 4, .label = "rojorhi.test.highSlot.input"}, &expected);
    auto output = (*device)->createBuffer({.size = 4,
                                           .storageWrite = true,
                                           .cpuReadback = true,
                                           .label = "rojorhi.test.highSlot.output"},
                                          nullptr);
    REQUIRE(input);
    REQUIRE(output);
    auto library = (*device)->loadShaderLibrary("Shaders/BufferBindingLimit");
    REQUIRE(library);
    auto pipeline = (*device)->createComputePipeline({.library = library->get(),
                                                      .computeEntry = "computeMain",
                                                      .threadsPerThreadgroup = {1, 1, 1},
                                                      .label = "rojorhi.test.highSlot"});
    REQUIRE(pipeline);
    auto& commands = (*device)->beginFrame();
    commands.beginComputePass("rojorhi.test.highSlot");
    commands.bindComputePipeline(**pipeline);
    commands.bindBuffer(CommandList::kMaxBufferBindings - 1, **input);
    commands.bindStorageBuffer(0, **output, StorageAccess::Write);
    commands.dispatch(1, 1, 1);
    commands.endComputePass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();
    uint32_t actual = 0;
    (*output)->readback(&actual, sizeof(actual));
    REQUIRE(actual == expected);
}
