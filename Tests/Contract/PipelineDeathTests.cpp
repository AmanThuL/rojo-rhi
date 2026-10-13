//----------------------------------------------------------------------------------------------------------------------
/// @file PipelineDeathTests.cpp
/// @brief Death tests for the valid-usage rules of shader libraries and pipelines, and for the pipeline sites of the format rules.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Support/Death.h"
#include "Support/NullLibrary.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0001 a stage names an empty entry point", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0001", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        (void)device.createPipeline(ComputePipelineDesc{ .compute = { library, "" } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0002 a stage names an entry point of another stage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0002", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        (void)device.createPipeline(ComputePipelineDesc{ .compute = { library, "vsMain" } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0003 loadShaderLibrary is given an empty blob", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0003", [] {
        Device device = *createDevice();
        (void)device.loadShaderLibrary({}, "death.library");
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0003 a color target format cannot be an attachment", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0003", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        FormatCapabilities capabilities = device.formatCapabilities(Format::RGBA8Unorm);
        capabilities.colorAttachment = false;
        core::coreOf(device).setFormatCapabilities(Format::RGBA8Unorm, capabilities);
        const std::array<ColorTarget, 1> colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
        (void)device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .colors = colors });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0003 blending is enabled for a format that cannot blend", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0003", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        FormatCapabilities capabilities = device.formatCapabilities(Format::RGBA8Unorm);
        capabilities.blend = false;
        core::coreOf(device).setFormatCapabilities(Format::RGBA8Unorm, capabilities);
        const std::array<ColorTarget, 1> colors{ ColorTarget{ .format = Format::RGBA8Unorm, .blend = { .enable = true } } };
        (void)device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .colors = colors });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0006 conservative rasterization is requested without the capability", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0006", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        core::coreOf(device).capabilities().conservativeRaster = false;
        GraphicsPipelineDesc desc{ .vertex = { library, "vsMain" } };
        desc.raster.conservative = true;
        (void)device.createPipeline(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0007 depthWrite is set without depthTest", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0007", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        GraphicsPipelineDesc desc{ .vertex = { library, "vsMain" }, .depthFormat = Format::D32Float };
        desc.depthStencil.depthWrite = true;
        (void)device.createPipeline(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0008 a graphics pipeline has neither vertex nor mesh", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0008", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        (void)device.createPipeline(GraphicsPipelineDesc{ .fragment = { library, "fsMain" } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0008 a graphics pipeline has both vertex and mesh", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0008", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        (void)device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .mesh = { library, "msMain" } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0009 a task entry is present without a mesh entry", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0009", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        (void)device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .task = { library, "tsMain" } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0010 a pipeline has more color targets than the limit", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0010", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        const std::vector<ColorTarget> colors(device.capabilities().limits.maxColorAttachments + 1, ColorTarget{ .format = Format::RGBA8Unorm });
        (void)device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .colors = colors });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0011 the depth format is a color format", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0011", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        (void)device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .depthFormat = Format::RGBA8Unorm });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0011 depthTest is set without a depth format", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0011", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        GraphicsPipelineDesc desc{ .vertex = { library, "vsMain" } };
        desc.depthStencil.depthTest = true;
        (void)device.createPipeline(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0011 stencilTest is set for a depth format without stencil", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0011", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        GraphicsPipelineDesc desc{ .vertex = { library, "vsMain" }, .depthFormat = Format::D32Float };
        desc.depthStencil.stencilTest = true;
        (void)device.createPipeline(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0003 a color format does not support the pipeline's sample count", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0003", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        FormatCapabilities capabilities = device.formatCapabilities(Format::RGBA8Unorm);
        capabilities.sampleCounts = 1;
        core::coreOf(device).setFormatCapabilities(Format::RGBA8Unorm, capabilities);
        const std::array<ColorTarget, 1> colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
        (void)device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .colors = colors, .samples = 4 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0003 the depth format does not support the pipeline's sample count", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0003", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        FormatCapabilities capabilities = device.formatCapabilities(Format::D32Float);
        capabilities.sampleCounts = 1;
        core::coreOf(device).setFormatCapabilities(Format::D32Float, capabilities);
        (void)device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .depthFormat = Format::D32Float, .samples = 4 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0013 a compute pipeline has no compute entry", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0013", [] {
        Device device = *createDevice();
        (void)device.createPipeline(ComputePipelineDesc{});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0002 a color target has Format::Undefined", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0002", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        const std::array<ColorTarget, 1> colors{ ColorTarget{} };
        (void)device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .colors = colors });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0003 a pipeline uses a depth format the device cannot attach", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0003", [] {
        Device device = *createDevice();
        const std::vector<std::byte> blob = test::standardLibrary();
        const ShaderLibrary library = *device.loadShaderLibrary(blob, "death.library");
        FormatCapabilities capabilities = device.formatCapabilities(Format::D32Float);
        capabilities.depthAttachment = false;
        core::coreOf(device).setFormatCapabilities(Format::D32Float, capabilities);
        (void)device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .depthFormat = Format::D32Float });
    });
}
