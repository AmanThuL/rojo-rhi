//----------------------------------------------------------------------------------------------------------------------
/// @file PipelineCoreTests.cpp
/// @brief Tests the records the core keeps for shader libraries and pipelines.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Pipelines.h"
#include "Support/NullLibrary.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    ShaderLibrary loadStandard(Device device)
    {
        const std::vector<std::byte> blob = test::standardLibrary();
        return *device.loadShaderLibrary(blob, "core.library");
    }

    //==================================================================================================================
    const core::PipelineRecord& recordOf(Device device, Pipeline pipeline)
    {
        return *core::coreOf(device).pool<Pipeline>().lookup(pipeline).record;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a shader library's record keeps its label and entries", "[core]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    const core::ShaderLibraryRecord* record = core::coreOf(device).pool<ShaderLibrary>().lookup(library).record;
    REQUIRE(record != nullptr);
    CHECK(record->label == "core.library");
    REQUIRE(record->entries.size() == 5);
    CHECK(record->entries[0].name == "vsMain");
    CHECK(record->entries[0].stage == core::ShaderStage::Vertex);
    CHECK(record->entries[4].stage == core::ShaderStage::Compute);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a shader library without a label is recorded as rojorhi.shaderlibrary.unnamed", "[core]")
{
    Device device = *createDevice();
    const std::vector<std::byte> blob = test::standardLibrary();
    const ShaderLibrary library = *device.loadShaderLibrary(blob, "");
    CHECK(core::coreOf(device).pool<ShaderLibrary>().lookup(library).record->label == "rojorhi.shaderlibrary.unnamed");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a graphics pipeline's record keeps its attachments, samples, stages and class", "[core]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    const std::array<ColorTarget, 2> colors{ ColorTarget{ .format = Format::RGBA16Float }, ColorTarget{ .format = Format::R8Unorm } };
    GraphicsPipelineDesc desc{ .vertex = { library, "vsMain" },
                               .fragment = { library, "fsMain" },
                               .primitive = PrimitiveClass::Line,
                               .colors = colors,
                               .depthFormat = Format::D32Float,
                               .samples = 4,
                               .label = "core.pipeline" };
    desc.raster.depthBias = true;
    const core::PipelineRecord& record = recordOf(device, *device.createPipeline(desc));
    CHECK(record.label == "core.pipeline");
    CHECK(record.kind == core::PipelineKind::Graphics);
    CHECK(record.stages == (core::stageBit(core::ShaderStage::Vertex) | core::stageBit(core::ShaderStage::Fragment)));
    CHECK(record.primitive == PrimitiveClass::Line);
    CHECK(record.colors == std::vector<Format>{ Format::RGBA16Float, Format::R8Unorm });
    CHECK(record.depthFormat == Format::D32Float);
    CHECK(record.samples == 4);
    CHECK(record.depthBias);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a mesh pipeline's record holds the task and mesh stages", "[core]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    const core::PipelineRecord& record =
        recordOf(device, *device.createPipeline(GraphicsPipelineDesc{ .task = { library, "tsMain" }, .mesh = { library, "msMain" } }));
    CHECK(record.stages == (core::stageBit(core::ShaderStage::Task) | core::stageBit(core::ShaderStage::Mesh)));
    CHECK(record.colors.empty());
    CHECK(record.depthFormat == Format::Undefined);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a compute pipeline's record is compute with only the compute stage and no label", "[core]")
{
    Device device = *createDevice();
    const ShaderLibrary library = loadStandard(device);
    const core::PipelineRecord& record = recordOf(device, *device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" } }));
    CHECK(record.label == "rojorhi.pipeline.unnamed");
    CHECK(record.kind == core::PipelineKind::Compute);
    CHECK(record.stages == core::stageBit(core::ShaderStage::Compute));
    destroyDevice(device);
}
