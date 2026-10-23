//----------------------------------------------------------------------------------------------------------------------
/// @file PipelineTests.cpp
/// @brief Tests shader libraries, pipelines and the pipeline cache against chapter 06.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Pipelines.h"
#include "EmbeddedShaders.h"
#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    struct Message
    {
        MessageSeverity severity;
        std::string text;
    };
    std::vector<Message> gMessages;

    //==================================================================================================================
    void collectMessage(MessageSeverity severity, std::string_view message, void*)
    {
        gMessages.push_back({ severity, std::string(message) });
    }

    //==================================================================================================================
    ShaderLibrary loadStandard(Device device)
    {
        const std::vector<std::byte> blob = test::standardLibrary();
        Result<ShaderLibrary> library = device.loadShaderLibrary(blob, "test.library");
        REQUIRE(library.has_value());
        return *library;
    }
} // namespace

//======================================================================================================================
TEST_CASE("loadShaderLibrary returns a library that destroy accepts", "[contract]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    CHECK(library.bits != 0);
    device.destroy(library);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("loadShaderLibrary returns an error for a blob not built for the backend", "[contract]")
{
    Device device = *createDevice();
    const std::vector<std::byte> blob = test::foreignLibrary();
    CHECK_FALSE(device.loadShaderLibrary(blob, "test.library").has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("loadShaderLibrary returns ShaderAbiMismatch for a library stamped with another ABI version", "[contract]")
{
    Device device = *createDevice();
    const std::vector<std::byte> blob = test::libraryStampedWith(core::kShaderAbiVersion + 1);
    const Result<ShaderLibrary> library = device.loadShaderLibrary(blob, "test.library");
    REQUIRE_FALSE(library.has_value());
    CHECK(library.error().category == ErrorCategory::ShaderAbiMismatch);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a failed loadShaderLibrary creates nothing to destroy", "[contract]")
{
    Device device = *createDevice();
    const std::vector<std::byte> blob = test::libraryStampedWith(core::kShaderAbiVersion + 1);
    const size_t before = core::coreOf(device).pool<ShaderLibrary>().liveCount();
    CHECK_FALSE(device.loadShaderLibrary(blob, "test.library").has_value());
    CHECK(core::coreOf(device).pool<ShaderLibrary>().liveCount() == before);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("createPipeline creates a vertex pipeline with a fragment stage", "[contract]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    const std::array<ColorTarget, 1> colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Result<Pipeline> pipeline = device.createPipeline(
        GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .colors = colors, .label = "test.pipeline" }
    );
    REQUIRE(pipeline.has_value());
    device.destroy(*pipeline);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("createPipeline creates a depth-only pipeline", "[contract]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    GraphicsPipelineDesc desc{ .vertex = { library, "vsMain" }, .depthFormat = Format::D32Float };
    desc.depthStencil.depthTest = true;
    desc.depthStencil.depthWrite = true;
    CHECK(device.createPipeline(desc).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("createPipeline creates a mesh pipeline with and without a task stage", "[contract]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    CHECK(device.createPipeline(GraphicsPipelineDesc{ .mesh = { library, "msMain" } }).has_value());
    CHECK(device.createPipeline(GraphicsPipelineDesc{ .task = { library, "tsMain" }, .mesh = { library, "msMain" }, .fragment = { library, "fsMain" } })
              .has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("createPipeline creates a multisampled pipeline with depth and stencil", "[contract]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    const std::array<ColorTarget, 2> colors{ ColorTarget{ .format = Format::RGBA16Float }, ColorTarget{ .format = Format::R8Unorm } };
    GraphicsPipelineDesc desc{
        .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .colors = colors, .depthFormat = Format::D32FloatS8, .samples = 4
    };
    desc.depthStencil.depthTest = true;
    desc.depthStencil.stencilTest = true;
    desc.raster.conservative = device.capabilities().conservativeRaster;
    CHECK(device.createPipeline(desc).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("createPipeline creates a compute pipeline", "[contract]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    const Result<Pipeline> pipeline = device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" }, .label = "test.compute" });
    REQUIRE(pipeline.has_value());
    device.destroy(*pipeline);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("createPipeline creates a compute pipeline from a rule-built library whose entry imports a shared module", "[contract]")
{
    Device device = *createDevice();
    const std::vector<std::byte> blob = test::shaderLibraryFile("SharedImport");
    const Result<ShaderLibrary> library = device.loadShaderLibrary(blob, "test.shared");
    REQUIRE(library.has_value());
    const Result<Pipeline> pipeline = device.createPipeline(ComputePipelineDesc{ .compute = { *library, "sharedMain" }, .label = "test.shared" });
    REQUIRE(pipeline.has_value());
    device.destroy(*pipeline);
    device.destroy(*library);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an embedded shader library loads and creates a compute pipeline", "[contract]")
{
    Device device = *createDevice();
    const Result<ShaderLibrary> library = device.loadShaderLibrary(rojoRHI::embedded::kStandardLibrary, "test.embedded");
    REQUIRE(library.has_value());
    const Result<Pipeline> pipeline = device.createPipeline(ComputePipelineDesc{ .compute = { *library, "csMain" }, .label = "test.embedded" });
    REQUIRE(pipeline.has_value());
    device.destroy(*pipeline);
    device.destroy(*library);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a pipeline is created from a name that need not outlive the call", "[contract]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    std::string name = "csMain";
    const Result<Pipeline> pipeline = device.createPipeline(ComputePipelineDesc{ .compute = { library, name } });
    name.assign("gone");
    CHECK(pipeline.has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("pipelineCacheData returns a blob a later device accepts", "[contract]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    REQUIRE(device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" } }).has_value());
    const Result<std::vector<std::byte>> data = device.pipelineCacheData();
    REQUIRE(data.has_value());
    destroyDevice(device);

    gMessages.clear();
    setMessageCallback(collectMessage, nullptr);
    Result<Device> second = createDevice(DeviceDesc{ .pipelineCache = *data });
    setMessageCallback(nullptr, nullptr);
    REQUIRE(second.has_value());
    CHECK(gMessages.empty());
    destroyDevice(*second);
}

//======================================================================================================================
TEST_CASE("a foreign pipeline cache blob is discarded with a message and is not an error", "[contract]")
{
    const std::array<std::byte, 4> foreign{ std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 }, std::byte{ 4 } };
    gMessages.clear();
    setMessageCallback(collectMessage, nullptr);
    Result<Device> device = createDevice(DeviceDesc{ .pipelineCache = foreign });
    setMessageCallback(nullptr, nullptr);
    REQUIRE(device.has_value());
    REQUIRE(gMessages.size() == 1);
    CHECK(gMessages[0].severity == MessageSeverity::Warning);
    destroyDevice(*device);
}

//======================================================================================================================
TEST_CASE("createDevice with an empty pipeline cache reports nothing", "[contract]")
{
    gMessages.clear();
    setMessageCallback(collectMessage, nullptr);
    Result<Device> device = createDevice();
    setMessageCallback(nullptr, nullptr);
    REQUIRE(device.has_value());
    CHECK(gMessages.empty());
    destroyDevice(*device);
}
