//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4PipelineTests.cpp
/// @brief Tests Metal 4 render, mesh and compute pipelines: their payloads, shared depth-stencil states, counts and lifetimes.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4PipelineState.h"
#include "ShaderList.h"
#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The one color target most cases draw to.
    constexpr std::array<ColorTarget, 1> kColor = { ColorTarget{ .format = Format::RGBA8Unorm } };

    //==================================================================================================================
    /// Loads the packaged conformance library `name` on `device`.
    ShaderLibrary load(Device& device, std::string_view name)
    {
        const Result<ShaderLibrary> library = device.loadShaderLibrary(test::shaderLibraryFile(name), std::string("test.") + std::string(name));
        REQUIRE(library.has_value());
        return *library;
    }

    //==================================================================================================================
    /// Returns the payload of `pipeline`, read as recording reads it.
    const metal4::PipelineState& stateOf(Device& device, Pipeline pipeline)
    {
        const auto* state = static_cast<const metal4::PipelineState*>(DeviceImpl::of(device).core.pool<Pipeline>().native(pipeline));
        REQUIRE(state != nullptr);
        return *state;
    }

    //==================================================================================================================
    /// Creates `desc` on `device`, failing the case with the error's message when creation fails.
    Pipeline create(Device& device, const GraphicsPipelineDesc& desc)
    {
        const Result<Pipeline> pipeline = device.createPipeline(desc);
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    //==================================================================================================================
    /// Creates a compute pipeline over `entry` of `library`, failing the case with the error's message when creation fails.
    Pipeline createCompute(Device& device, ShaderLibrary library, std::string_view entry)
    {
        const Result<Pipeline> pipeline = device.createPipeline(ComputePipelineDesc{ .compute = { library, entry }, .label = "test.compute" });
        INFO(entry);
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    //==================================================================================================================
    /// The standard vertex and fragment pipeline over one `RGBA8Unorm` target, with `raster` and `depthStencil`.
    GraphicsPipelineDesc standardDesc(ShaderLibrary library, const RasterState& raster = {}, const DepthStencilState& depthStencil = {})
    {
        return GraphicsPipelineDesc{
            .vertex = { library, "vsMain" },
            .fragment = { library, "fsMain" },
            .raster = raster,
            .depthStencil = depthStencil,
            .colors = kColor,
            .depthFormat = depthStencil.depthTest || depthStencil.stencilTest ? Format::D32FloatS8 : Format::Undefined,
            .label = "test.standard",
        };
    }

    //==================================================================================================================
    /// Returns `object`'s retain count.
    NS::UInteger retainsOf(const NS::Object* object)
    {
        return object->retainCount();
    }
} // namespace

//======================================================================================================================
TEST_CASE("a vertex pipeline's payload holds its raster state, list topology and depth-stencil state", "[metal4]")
{
    Device device = *createDevice({});
    const ShaderLibrary library = *device.loadShaderLibrary(test::standardLibrary(), "test.standard");

    const Pipeline plain = create(device, standardDesc(library));
    const metal4::PipelineState& base = stateOf(device, plain);
    CHECK(base.kind == core::PipelineKind::Graphics);
    CHECK_FALSE(base.mesh);
    CHECK(base.render != nullptr);
    CHECK(base.compute == nullptr);
    CHECK(base.depthStencil == nullptr);
    CHECK(base.cull == MTL::CullModeNone);
    CHECK(base.winding == MTL::WindingCounterClockwise);
    CHECK(base.fill == MTL::TriangleFillModeFill);
    CHECK(base.depthClip == MTL::DepthClipModeClip);
    CHECK_FALSE(base.depthBias);
    CHECK(base.listType == MTL::PrimitiveTypeTriangle);

    for (const auto& [cull, native] : { std::pair{ CullMode::Front, MTL::CullModeFront }, std::pair{ CullMode::Back, MTL::CullModeBack } })
    {
        CHECK(stateOf(device, create(device, standardDesc(library, { .cull = cull }))).cull == native);
    }
    CHECK(stateOf(device, create(device, standardDesc(library, { .frontFace = FrontFace::Clockwise }))).winding == MTL::WindingClockwise);
    CHECK(stateOf(device, create(device, standardDesc(library, { .fill = FillMode::Wireframe }))).fill == MTL::TriangleFillModeLines);
    CHECK(stateOf(device, create(device, standardDesc(library, { .depthClip = false }))).depthClip == MTL::DepthClipModeClamp);
    CHECK(stateOf(device, create(device, standardDesc(library, { .depthBias = true }))).depthBias);
    for (const auto& [primitive, native] : { std::pair{ PrimitiveClass::Point, MTL::PrimitiveTypePoint },
                                             std::pair{ PrimitiveClass::Line, MTL::PrimitiveTypeLine },
                                             std::pair{ PrimitiveClass::Triangle, MTL::PrimitiveTypeTriangle } })
    {
        GraphicsPipelineDesc desc = standardDesc(library);
        desc.primitive = primitive;
        CHECK(stateOf(device, create(device, desc)).listType == native);
    }
    const DepthStencilState tested{ .depthTest = true, .depthWrite = true };
    const metal4::PipelineState& depth = stateOf(device, create(device, standardDesc(library, {}, tested)));
    CHECK(depth.depthStencil != nullptr);
    CHECK(depth.depthStencil == DeviceImpl::of(device).depthStencils.get(DeviceImpl::of(device).device, tested, DeviceImpl::of(device).core.counts()));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("pipelines with equal depth-stencil states share one native state, and one testing neither has none", "[metal4]")
{
    Device device = *createDevice({});
    DeviceImpl& impl = DeviceImpl::of(device);
    const ShaderLibrary library = *device.loadShaderLibrary(test::standardLibrary(), "test.standard");
    const DepthStencilState depth{ .depthTest = true, .depthWrite = true };
    const DepthStencilState less{ .depthTest = true, .depthWrite = true, .depthCompare = CompareOp::Less };
    const DepthStencilState stencil{
        .stencilTest = true,
        .stencilReadMask = 0x0F,
        .stencilWriteMask = 0xF0,
        .front = { .pass = StencilOp::Replace, .compare = CompareOp::Equal },
        .back = { .fail = StencilOp::Invert, .compare = CompareOp::NotEqual },
    };

    const uint64_t createsBefore = device.counters().nativeCreateCalls;
    MTL::DepthStencilState* first = stateOf(device, create(device, standardDesc(library, {}, depth))).depthStencil;
    MTL::DepthStencilState* second = stateOf(device, create(device, standardDesc(library, { .cull = CullMode::Back }, depth))).depthStencil;
    MTL::DepthStencilState* other = stateOf(device, create(device, standardDesc(library, {}, less))).depthStencil;
    MTL::DepthStencilState* stenciled = stateOf(device, create(device, standardDesc(library, {}, stencil))).depthStencil;
    CHECK(first != nullptr);
    CHECK(first == second);
    CHECK(other != nullptr);
    CHECK(other != first);
    CHECK(stenciled != nullptr);
    CHECK(stenciled != first);
    CHECK(stenciled != other);
    CHECK(stateOf(device, create(device, standardDesc(library))).depthStencil == nullptr);
    // A depth format alone tests nothing.
    GraphicsPipelineDesc untested = standardDesc(library);
    untested.depthFormat = Format::D32Float;
    CHECK(stateOf(device, create(device, untested)).depthStencil == nullptr);
    CHECK(impl.depthStencils.size() == 3);
    // Six pipelines, and one native creation for each of the three distinct states the cache missed.
    CHECK(device.counters().nativeCreateCalls == createsBefore + 6 + 3);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the depth-stencil cache counts each native creation and release", "[metal4]")
{
    Device device = *createDevice({});
    core::DeviceCounts counts;
    metal4::DepthStencilCache cache;
    {
        const metal4::AutoreleasePool pool;
        const DepthStencilState depth{ .depthTest = true };
        CHECK(cache.get(DeviceImpl::of(device).device, depth, counts) == cache.get(DeviceImpl::of(device).device, depth, counts));
        CHECK(cache.get(DeviceImpl::of(device).device, { .depthTest = true, .depthCompare = CompareOp::Less }, counts) != nullptr);
        CHECK(cache.get(DeviceImpl::of(device).device, {}, counts) == nullptr);
    }
    CHECK(core::DeviceCounts::read(counts.nativeCreateCalls) == 2);
    {
        const metal4::AutoreleasePool pool;
        cache.release(counts);
    }
    CHECK(core::DeviceCounts::read(counts.nativeCreateCalls) == 4);
    CHECK(cache.size() == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a compute pipeline's payload holds the library's threadgroup size", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    const ShaderLibrary library = load(device, "AbiCompute");
    const metal4::PipelineState& state = stateOf(device, createCompute(device, library, "computeMain"));
    CHECK(state.kind == core::PipelineKind::Compute);
    CHECK(state.compute != nullptr);
    CHECK(state.render == nullptr);
    CHECK(state.depthStencil == nullptr);
    CHECK(state.computeThreads.width == 1);
    CHECK(state.computeThreads.height == 1);
    CHECK(state.computeThreads.depth == 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a mesh pipeline holds the object and mesh sizes of its library", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    const ShaderLibrary library = load(device, "AbiMesh");
    const metal4::PipelineState& state =
        stateOf(device, create(device, GraphicsPipelineDesc{ .task = { library, "taskMain" }, .mesh = { library, "meshMain" }, .label = "test.mesh" }));
    CHECK(state.kind == core::PipelineKind::Graphics);
    CHECK(state.mesh);
    CHECK(state.render != nullptr);
    CHECK(state.objectThreads.width == 1);
    CHECK(state.objectThreads.height == 1);
    CHECK(state.objectThreads.depth == 1);
    CHECK(state.meshThreads.width == 3);
    CHECK(state.meshThreads.height == 1);
    CHECK(state.meshThreads.depth == 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("every conformance entry makes a pipeline", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    const ShaderLibrary textures = load(device, "AbiTextures");
    for (const std::string_view entry : { "texture1DMain",
                                          "texture1DArrayMain",
                                          "texture2DMain",
                                          "texture2DArrayMain",
                                          "texture3DMain",
                                          "textureCubeMain",
                                          "textureCubeArrayMain",
                                          "compareMain",
                                          "texture2DMSMain",
                                          "texture2DMSArrayMain",
                                          "rwTexture1DMain",
                                          "rwTexture1DArrayMain",
                                          "rwTexture2DMain",
                                          "rwTexture2DArrayMain",
                                          "rwTexture3DMain" })
    {
        createCompute(device, textures, entry);
    }
    createCompute(device, load(device, "AbiCompute"), "computeMain");

    // A void vertex entry with a color target and no depth: rasterization is off.
    const ShaderLibrary vertex = load(device, "AbiVertex");
    create(device, GraphicsPipelineDesc{ .vertex = { vertex, "vertexMain" }, .colors = kColor, .label = "test.abiVertex" });
    // With a depth format the first compile rasterizes, which Metal refuses for a void vertex entry; the second does not.
    create(device, GraphicsPipelineDesc{ .vertex = { vertex, "vertexMain" }, .depthFormat = Format::D32Float, .label = "test.abiVertexDepth" });
    const ShaderLibrary mesh = load(device, "AbiMesh");
    create(device, GraphicsPipelineDesc{ .task = { mesh, "taskMain" }, .mesh = { mesh, "meshMain" }, .colors = kColor, .label = "test.abiMesh" });

    const ShaderLibrary standard = *device.loadShaderLibrary(test::standardLibrary(), "test.standard");
    // A vertex entry that returns a position with nothing observable to rasterize: Metal refuses it without rasterization,
    // so the second compile rasterizes.
    create(device, GraphicsPipelineDesc{ .vertex = { standard, "vsMain" }, .colors = kColor, .label = "test.vsOnly" });
    create(device, GraphicsPipelineDesc{ .vertex = { standard, "vsMain" }, .depthFormat = Format::D32Float, .label = "test.depthOnly" });
    create(device, GraphicsPipelineDesc{ .mesh = { standard, "msMain" }, .colors = kColor, .label = "test.msOnly" });
    createCompute(device, standard, "csMain");
    create(device, standardDesc(standard));
    const metal4::PipelineState& full = stateOf(
        device,
        create(
            device,
            GraphicsPipelineDesc{
                .task = { standard, "tsMain" }, .mesh = { standard, "msMain" }, .fragment = { standard, "fsMain" }, .colors = kColor, .label = "test.meshFull" }
        )
    );
    CHECK(full.mesh);
    CHECK(full.objectThreads.width == 1);
    CHECK(full.meshThreads.width == 1);

    // Four samples on a resolvable color format, and a depth-stencil format with both tests.
    GraphicsPipelineDesc multisampled = standardDesc(standard);
    multisampled.samples = 4;
    create(device, multisampled);
    create(device, standardDesc(standard, {}, { .depthTest = true, .stencilTest = true, .front = { .pass = StencilOp::IncrementWrap } }));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("createPipeline counts the pipeline and its compile time", "[metal4]")
{
    Device device = *createDevice({});
    const ShaderLibrary library = *device.loadShaderLibrary(test::standardLibrary(), "test.standard");
    const Counters before = device.counters();
    create(device, standardDesc(library));
    const Counters graphics = device.counters();
    CHECK(graphics.pipelinesCreated == before.pipelinesCreated + 1);
    CHECK(graphics.pipelineCompileNanoseconds > before.pipelineCompileNanoseconds);
    createCompute(device, library, "csMain");
    const Counters compute = device.counters();
    CHECK(compute.pipelinesCreated == graphics.pipelinesCreated + 1);
    CHECK(compute.pipelineCompileNanoseconds > graphics.pipelineCompileNanoseconds);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("four threads create pipelines at once and share depth-stencil states", "[metal4]")
{
    Device device = *createDevice({});
    const ShaderLibrary library = *device.loadShaderLibrary(test::standardLibrary(), "test.standard");
    const DepthStencilState depth{ .depthTest = true, .depthWrite = true };
    std::array<std::vector<Pipeline>, 4> created;
    std::vector<std::thread> threads;
    for (size_t t = 0; t < created.size(); ++t)
    {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 4; ++i)
            {
                const Result<Pipeline> pipeline = device.createPipeline(standardDesc(library, {}, depth));
                if (pipeline)
                {
                    created[t].push_back(*pipeline);
                }
                const Result<Pipeline> compute = device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" } });
                if (compute)
                {
                    created[t].push_back(*compute);
                }
            }
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    MTL::DepthStencilState* shared = nullptr;
    for (const std::vector<Pipeline>& pipelines : created)
    {
        CHECK(pipelines.size() == 8);
        for (const Pipeline pipeline : pipelines)
        {
            const metal4::PipelineState& state = stateOf(device, pipeline);
            if (state.kind == core::PipelineKind::Graphics)
            {
                shared = shared == nullptr ? state.depthStencil : shared;
                CHECK(state.depthStencil == shared);
            }
            device.destroy(pipeline);
        }
    }
    CHECK(shared != nullptr);
    CHECK(DeviceImpl::of(device).depthStencils.size() == 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a destroyed pipeline is released when its frame retires; device teardown releases the depth-stencil states", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    core::DeviceCore& core = DeviceImpl::of(device).core;
    const ShaderLibrary library = *device.loadShaderLibrary(test::standardLibrary(), "test.standard");
    const size_t before = core.pool<Pipeline>().liveCount();
    const Pipeline pipeline = create(device, standardDesc(library, {}, { .depthTest = true }));
    const metal4::PipelineState& state = stateOf(device, pipeline);
    MTL::RenderPipelineState* render = state.render->retain();
    MTL::DepthStencilState* depthStencil = state.depthStencil->retain();
    const NS::UInteger renderHeld = retainsOf(render);
    const NS::UInteger depthStencilHeld = retainsOf(depthStencil);
    CHECK(core.pool<Pipeline>().liveCount() == before + 1);

    device.destroy(pipeline);
    for (int frame = 0; frame < 3; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    CHECK(core.pool<Pipeline>().liveCount() == before);
    CHECK(retainsOf(render) == renderHeld - 1);
    // The cache keeps the depth-stencil state after its pipeline retires, for the next pipeline that tests the same.
    CHECK(retainsOf(depthStencil) == depthStencilHeld);

    destroyDevice(device);
    CHECK(retainsOf(depthStencil) == depthStencilHeld - 1);
    render->release();
    depthStencil->release();
}

//======================================================================================================================
TEST_CASE("a pipeline whose fragment reads a varying its vertex does not write is DriverFailure", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    const ShaderLibrary standard = *device.loadShaderLibrary(test::standardLibrary(), "test.standard");
    const ShaderLibrary fragment = load(device, "AbiFragment");
    const size_t before = DeviceImpl::of(device).core.pool<Pipeline>().liveCount();
    const Result<Pipeline> pipeline = device.createPipeline(
        GraphicsPipelineDesc{ .vertex = { standard, "vsMain" }, .fragment = { fragment, "fragmentMain" }, .colors = kColor, .label = "test.mismatch" }
    );
    REQUIRE_FALSE(pipeline.has_value());
    CHECK(pipeline.error().category == ErrorCategory::DriverFailure);
    CHECK_THAT(pipeline.error().message, Catch::Matchers::ContainsSubstring("the pipeline \"test.mismatch\" was not created"));
    CHECK(DeviceImpl::of(device).core.pool<Pipeline>().liveCount() == before);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a mesh pipeline with color targets, a depth format and no fragment entry is DriverFailure", "[metal4]")
{
    Device device = *createDevice({});
    const ShaderLibrary standard = *device.loadShaderLibrary(test::standardLibrary(), "test.standard");
    const Result<Pipeline> refused = device.createPipeline(
        GraphicsPipelineDesc{ .mesh = { standard, "msMain" }, .colors = kColor, .depthFormat = Format::D32Float, .label = "test.meshColorsDepth" }
    );
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().category == ErrorCategory::DriverFailure);
    CHECK_THAT(refused.error().message, Catch::Matchers::ContainsSubstring("needs a fragment entry"));
    // Without colors, or without depth, the same entry makes a pipeline.
    create(device, GraphicsPipelineDesc{ .mesh = { standard, "msMain" }, .depthFormat = Format::D32Float, .label = "test.meshDepth" });
    create(device, GraphicsPipelineDesc{ .mesh = { standard, "msMain" }, .colors = kColor, .label = "test.meshColors" });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("color targets reach the descriptor with their write masks, blend factors and operations", "[metal4]")
{
    const std::array<ColorTarget, 3> targets = {
        ColorTarget{ .format = Format::RGBA8Unorm, .writeMask = ColorWriteMask::R },
        ColorTarget{ .format = Format::RGBA8Unorm, .writeMask = ColorWriteMask::A },
        ColorTarget{ .format = Format::RGBA16Float,
                     .blend = { .enable = true,
                                .srcColor = BlendFactor::SrcAlpha,
                                .dstColor = BlendFactor::OneMinusSrcAlpha,
                                .colorOp = BlendOp::ReverseSubtract,
                                .srcAlpha = BlendFactor::Constant,
                                .dstAlpha = BlendFactor::OneMinusConstant,
                                .alphaOp = BlendOp::Max },
                     .writeMask = ColorWriteMask::R | ColorWriteMask::A },
    };
    {
        const metal4::AutoreleasePool pool;
        MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
        metal4::describeColors(descriptor->colorAttachments(), targets);
        MTL4::RenderPipelineColorAttachmentDescriptor* red = descriptor->colorAttachments()->object(0);
        CHECK(red->pixelFormat() == MTL::PixelFormatRGBA8Unorm);
        CHECK(red->writeMask() == MTL::ColorWriteMaskRed);
        CHECK(red->blendingState() == MTL4::BlendStateDisabled);
        MTL4::RenderPipelineColorAttachmentDescriptor* alpha = descriptor->colorAttachments()->object(1);
        CHECK(alpha->writeMask() == MTL::ColorWriteMaskAlpha);
        MTL4::RenderPipelineColorAttachmentDescriptor* blended = descriptor->colorAttachments()->object(2);
        CHECK(blended->pixelFormat() == MTL::PixelFormatRGBA16Float);
        CHECK(blended->writeMask() == (MTL::ColorWriteMaskRed | MTL::ColorWriteMaskAlpha));
        CHECK(blended->blendingState() == MTL4::BlendStateEnabled);
        CHECK(blended->sourceRGBBlendFactor() == MTL::BlendFactorSourceAlpha);
        CHECK(blended->destinationRGBBlendFactor() == MTL::BlendFactorOneMinusSourceAlpha);
        CHECK(blended->rgbBlendOperation() == MTL::BlendOperationReverseSubtract);
        CHECK(blended->sourceAlphaBlendFactor() == MTL::BlendFactorBlendColor);
        CHECK(blended->destinationAlphaBlendFactor() == MTL::BlendFactorOneMinusBlendColor);
        CHECK(blended->alphaBlendOperation() == MTL::BlendOperationMax);
        descriptor->release();
    }
    // Metal accepts the same targets in a pipeline.
    Device device = *createDevice({});
    const ShaderLibrary library = *device.loadShaderLibrary(test::standardLibrary(), "test.standard");
    GraphicsPipelineDesc desc = standardDesc(library);
    desc.colors = targets;
    create(device, desc);
    destroyDevice(device);
}
