//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12PipelineTests.cpp
/// @brief Tests D3D12 pipelines: the state streams they are built from, their payloads, counts and lifetimes.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Records.h"
#include "D3D12Device.h"
#include "D3D12PipelineState.h"
#include "ShaderList.h"
#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The one colour target most cases draw to.
    constexpr std::array<ColorTarget, 1> kColor = { ColorTarget{ .format = Format::RGBA8Unorm } };

    /// Bytes standing in for each stage's DXIL in the stream cases, which never reach the runtime.
    constexpr std::array<std::byte, 8> kVertexBytes = {};
    constexpr std::array<std::byte, 8> kTaskBytes = {};
    constexpr std::array<std::byte, 8> kMeshBytes = {};
    constexpr std::array<std::byte, 8> kFragmentBytes = {};
    constexpr std::array<std::byte, 8> kComputeBytes = {};

    //==================================================================================================================
    /// Returns bytecode over `bytes`.
    D3D12_SHADER_BYTECODE bytecodeOf(const std::array<std::byte, 8>& bytes)
    {
        return D3D12_SHADER_BYTECODE{ bytes.data(), bytes.size() };
    }

    //==================================================================================================================
    /// The stand-in shaders of a vertex pipeline with a fragment entry.
    d3d12::PipelineShaders vertexShaders()
    {
        return d3d12::PipelineShaders{ .vertex = bytecodeOf(kVertexBytes), .fragment = bytecodeOf(kFragmentBytes) };
    }

    //==================================================================================================================
    /// Returns the value of `stream`'s subobject of `type`, failing the case when the stream has none.
    template<class Value> Value valueOf(const d3d12::PipelineStream& stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
    {
        const std::optional<Value> value = stream.find<Value>(type);
        INFO("subobject type " << static_cast<int>(type));
        REQUIRE(value.has_value());
        return *value;
    }

    //==================================================================================================================
    /// Creates a device with two frames in flight, failing the case when it cannot.
    Device makeDevice()
    {
        DeviceDesc desc;
        desc.framesInFlight = 2;
        Result<Device> created = createDevice(desc);
        INFO((created ? std::string() : created.error().message));
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Loads the packaged conformance library `name` on `device`.
    ShaderLibrary load(Device& device, std::string_view name)
    {
        const Result<ShaderLibrary> library = device.loadShaderLibrary(test::shaderLibraryFile(name), std::string("test.") + std::string(name));
        INFO(name);
        REQUIRE(library.has_value());
        return *library;
    }

    //==================================================================================================================
    /// Loads the standard library on `device`.
    ShaderLibrary loadStandard(Device& device)
    {
        const Result<ShaderLibrary> library = device.loadShaderLibrary(test::standardLibrary(), "test.standard");
        REQUIRE(library.has_value());
        return *library;
    }

    //==================================================================================================================
    /// Returns the payload of `pipeline`, read as recording reads it.
    const d3d12::PipelineState& stateOf(Device& device, Pipeline pipeline)
    {
        const auto* state = static_cast<const d3d12::PipelineState*>(DeviceImpl::of(device).core.pool<Pipeline>().native(pipeline));
        REQUIRE(state != nullptr);
        REQUIRE(state->pso != nullptr);
        return *state;
    }

    //==================================================================================================================
    /// Creates `desc` on `device`, failing the case with the error's message when creation fails.
    Pipeline create(Device& device, const GraphicsPipelineDesc& desc)
    {
        const Result<Pipeline> pipeline = device.createPipeline(desc);
        INFO(desc.label);
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
    /// The standard vertex and fragment pipeline over one `RGBA8Unorm` target, with `raster`.
    GraphicsPipelineDesc standardDesc(ShaderLibrary library, const RasterState& raster = {})
    {
        return GraphicsPipelineDesc{
            .vertex = { library, "vsMain" },
            .fragment = { library, "fsMain" },
            .raster = raster,
            .colors = kColor,
            .label = "test.standard",
        };
    }
} // namespace

//======================================================================================================================
TEST_CASE("graphicsStream names the global root signature, an empty input layout and IndependentBlendEnable", "[d3d12]")
{
    Device device = makeDevice();
    ID3D12RootSignature* global = DeviceImpl::of(device).roots.global();
    REQUIRE(global != nullptr);
    const d3d12::PipelineStream stream = d3d12::graphicsStream(global, GraphicsPipelineDesc{ .colors = kColor }, vertexShaders());

    CHECK(valueOf<ID3D12RootSignature*>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) == global);
    const auto layout = valueOf<D3D12_INPUT_LAYOUT_DESC>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT);
    CHECK(layout.NumElements == 0);
    CHECK(layout.pInputElementDescs == nullptr);
    const auto blend = valueOf<D3D12_BLEND_DESC>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND);
    CHECK(blend.IndependentBlendEnable == TRUE);
    CHECK(blend.AlphaToCoverageEnable == FALSE);
    CHECK(valueOf<D3D12_SHADER_BYTECODE>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS).pShaderBytecode == kVertexBytes.data());
    CHECK(valueOf<D3D12_SHADER_BYTECODE>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS).pShaderBytecode == kFragmentBytes.data());
    CHECK(valueOf<UINT>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK) == std::numeric_limits<UINT>::max());
    const auto samples = valueOf<DXGI_SAMPLE_DESC>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC);
    CHECK(samples.Count == 1);
    CHECK(samples.Quality == 0);
    CHECK(valueOf<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY) == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    const auto depthStencil = valueOf<D3D12_DEPTH_STENCIL_DESC2>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2);
    CHECK(depthStencil.DepthEnable == FALSE);
    CHECK(depthStencil.StencilEnable == FALSE);
    CHECK(depthStencil.DepthBoundsTestEnable == FALSE);
    const auto raster = valueOf<D3D12_RASTERIZER_DESC2>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2);
    CHECK(raster.CullMode == D3D12_CULL_MODE_NONE);
    CHECK(raster.FillMode == D3D12_FILL_MODE_SOLID);
    CHECK(raster.FrontCounterClockwise == TRUE);
    CHECK(raster.DepthClipEnable == TRUE);
    CHECK(raster.LineRasterizationMode == D3D12_LINE_RASTERIZATION_MODE_ALIASED);
    CHECK(raster.ConservativeRaster == D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF);
    CHECK_FALSE(stream.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE));
    CHECK_FALSE(stream.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS));

    const D3D12_PIPELINE_STATE_STREAM_DESC desc = stream.desc();
    CHECK(desc.SizeInBytes > 0);
    CHECK(desc.SizeInBytes % sizeof(void*) == 0);
    CHECK(reinterpret_cast<uintptr_t>(desc.pPipelineStateSubobjectStream) % sizeof(void*) == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("computeStream names the global root signature and the compute entry alone", "[d3d12]")
{
    Device device = makeDevice();
    ID3D12RootSignature* global = DeviceImpl::of(device).roots.global();
    const d3d12::PipelineStream stream = d3d12::computeStream(global, bytecodeOf(kComputeBytes));
    CHECK(valueOf<ID3D12RootSignature*>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) == global);
    const auto compute = valueOf<D3D12_SHADER_BYTECODE>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS);
    CHECK(compute.pShaderBytecode == kComputeBytes.data());
    CHECK(compute.BytecodeLength == kComputeBytes.size());
    CHECK_FALSE(stream.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND));
    CHECK_FALSE(stream.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS));
    // Two subobjects: the root signature (8 bytes of type and padding, 8 of pointer) and CS (8, then 16 of bytecode).
    CHECK(stream.desc().SizeInBytes == 40);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a depthBias pipeline carries D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS and one without does not", "[d3d12]")
{
    Device device = makeDevice();
    ID3D12RootSignature* global = DeviceImpl::of(device).roots.global();
    const d3d12::PipelineStream biased = d3d12::graphicsStream(global, GraphicsPipelineDesc{ .raster = { .depthBias = true } }, vertexShaders());
    const d3d12::PipelineStream plain = d3d12::graphicsStream(global, GraphicsPipelineDesc{}, vertexShaders());
    CHECK(valueOf<D3D12_PIPELINE_STATE_FLAGS>(biased, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS) == D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS);
    CHECK(valueOf<D3D12_PIPELINE_STATE_FLAGS>(plain, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS) == D3D12_PIPELINE_STATE_FLAG_NONE);
    // The bias itself comes from the command list.
    const auto raster = valueOf<D3D12_RASTERIZER_DESC2>(biased, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2);
    CHECK(raster.DepthBias == 0.0f);
    CHECK(raster.DepthBiasClamp == 0.0f);
    CHECK(raster.SlopeScaledDepthBias == 0.0f);

    const ShaderLibrary library = loadStandard(device);
    CHECK(stateOf(device, create(device, standardDesc(library, { .depthBias = true }))).depthBias);
    CHECK_FALSE(stateOf(device, create(device, standardDesc(library))).depthBias);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("raster state reaches the rasterizer description", "[d3d12]")
{
    const D3D12_RASTERIZER_DESC2 culled =
        d3d12::rasterizerDesc({ .cull = CullMode::Back, .frontFace = FrontFace::Clockwise, .fill = FillMode::Wireframe, .depthClip = false });
    CHECK(culled.CullMode == D3D12_CULL_MODE_BACK);
    CHECK(culled.FrontCounterClockwise == FALSE);
    CHECK(culled.FillMode == D3D12_FILL_MODE_WIREFRAME);
    CHECK(culled.DepthClipEnable == FALSE);
    CHECK(d3d12::rasterizerDesc({ .cull = CullMode::Front }).CullMode == D3D12_CULL_MODE_FRONT);
    CHECK(d3d12::rasterizerDesc({ .conservative = true }).ConservativeRaster == D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON);
    CHECK(d3d12::topologyType(PrimitiveClass::Point) == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
    CHECK(d3d12::topologyType(PrimitiveClass::Line) == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
    CHECK(d3d12::listTopology(PrimitiveClass::Point) == D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    CHECK(d3d12::listTopology(PrimitiveClass::Line) == D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    CHECK(d3d12::listTopology(PrimitiveClass::Triangle) == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

//======================================================================================================================
TEST_CASE("colour targets map to RTVFormats in order and an sRGB target keeps its sRGB format", "[d3d12]")
{
    const std::array<ColorTarget, 3> targets = {
        ColorTarget{ .format = Format::RGBA16Float, .writeMask = ColorWriteMask::R | ColorWriteMask::A },
        ColorTarget{ .format = Format::RGBA8Unorm_sRGB },
        ColorTarget{ .format = Format::BGRA8Unorm, .writeMask = ColorWriteMask::G | ColorWriteMask::B },
    };
    const D3D12_RT_FORMAT_ARRAY formats = d3d12::renderTargetFormats(targets);
    CHECK(formats.NumRenderTargets == 3);
    CHECK(formats.RTFormats[0] == DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK(formats.RTFormats[1] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    CHECK(formats.RTFormats[2] == DXGI_FORMAT_B8G8R8A8_UNORM);
    for (size_t i = 3; i < std::size(formats.RTFormats); ++i)
    {
        CHECK(formats.RTFormats[i] == DXGI_FORMAT_UNKNOWN);
    }
    const D3D12_BLEND_DESC blend = d3d12::blendDesc(targets);
    CHECK(blend.RenderTarget[0].RenderTargetWriteMask == (D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_ALPHA));
    CHECK(blend.RenderTarget[1].RenderTargetWriteMask == D3D12_COLOR_WRITE_ENABLE_ALL);
    CHECK(blend.RenderTarget[2].RenderTargetWriteMask == (D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE));
    CHECK(d3d12::writeMask(ColorWriteMask::None) == 0);

    Device device = makeDevice();
    const d3d12::PipelineStream stream =
        d3d12::graphicsStream(DeviceImpl::of(device).roots.global(), GraphicsPipelineDesc{ .colors = targets }, vertexShaders());
    const auto streamed = valueOf<D3D12_RT_FORMAT_ARRAY>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS);
    CHECK(streamed.NumRenderTargets == 3);
    CHECK(streamed.RTFormats[1] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const ShaderLibrary library = loadStandard(device);
    GraphicsPipelineDesc desc = standardDesc(library);
    desc.colors = targets;
    create(device, desc);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a D32Float pipeline's DSVFormat is D32_FLOAT and a D32FloatS8 pipeline's D32_FLOAT_S8X24_UINT", "[d3d12]")
{
    Device device = makeDevice();
    ID3D12RootSignature* global = DeviceImpl::of(device).roots.global();
    const auto dsvOf = [&](Format format) {
        return valueOf<DXGI_FORMAT>(
            d3d12::graphicsStream(global, GraphicsPipelineDesc{ .depthFormat = format }, vertexShaders()),
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT
        );
    };
    CHECK(dsvOf(Format::D32Float) == DXGI_FORMAT_D32_FLOAT);
    CHECK(dsvOf(Format::D32FloatS8) == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    CHECK(dsvOf(Format::Undefined) == DXGI_FORMAT_UNKNOWN);

    const DepthStencilState state{
        .depthTest = true,
        .depthWrite = true,
        .depthCompare = CompareOp::Less,
        .stencilTest = true,
        .stencilReadMask = 0x0F,
        .stencilWriteMask = 0xF0,
        .front = { .fail = StencilOp::Zero, .depthFail = StencilOp::Replace, .pass = StencilOp::IncrementWrap, .compare = CompareOp::Equal },
        .back = { .fail = StencilOp::Invert, .depthFail = StencilOp::DecrementClamp, .pass = StencilOp::IncrementClamp, .compare = CompareOp::NotEqual },
    };
    const D3D12_DEPTH_STENCIL_DESC2 native = d3d12::depthStencilDesc(state);
    CHECK(native.DepthEnable == TRUE);
    CHECK(native.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL);
    CHECK(native.DepthFunc == D3D12_COMPARISON_FUNC_LESS);
    CHECK(native.StencilEnable == TRUE);
    CHECK(native.FrontFace.StencilFailOp == D3D12_STENCIL_OP_ZERO);
    CHECK(native.FrontFace.StencilDepthFailOp == D3D12_STENCIL_OP_REPLACE);
    CHECK(native.FrontFace.StencilPassOp == D3D12_STENCIL_OP_INCR);
    CHECK(native.FrontFace.StencilFunc == D3D12_COMPARISON_FUNC_EQUAL);
    CHECK(native.FrontFace.StencilReadMask == 0x0F);
    CHECK(native.FrontFace.StencilWriteMask == 0xF0);
    CHECK(native.BackFace.StencilFailOp == D3D12_STENCIL_OP_INVERT);
    CHECK(native.BackFace.StencilDepthFailOp == D3D12_STENCIL_OP_DECR_SAT);
    CHECK(native.BackFace.StencilPassOp == D3D12_STENCIL_OP_INCR_SAT);
    CHECK(native.BackFace.StencilFunc == D3D12_COMPARISON_FUNC_NOT_EQUAL);
    CHECK(native.BackFace.StencilReadMask == 0x0F);
    CHECK(native.DepthBoundsTestEnable == FALSE);

    const ShaderLibrary library = loadStandard(device);
    GraphicsPipelineDesc depth = standardDesc(library);
    depth.depthStencil = { .depthTest = true, .depthWrite = true };
    depth.depthFormat = Format::D32Float;
    create(device, depth);
    GraphicsPipelineDesc stencil = standardDesc(library);
    stencil.depthStencil = state;
    stencil.depthFormat = Format::D32FloatS8;
    create(device, stencil);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("alpha-slot blend factors use alpha forms", "[d3d12]")
{
    CHECK(d3d12::alphaBlend(BlendFactor::SrcColor) == D3D12_BLEND_SRC_ALPHA);
    CHECK(d3d12::alphaBlend(BlendFactor::OneMinusSrcColor) == D3D12_BLEND_INV_SRC_ALPHA);
    CHECK(d3d12::alphaBlend(BlendFactor::DstColor) == D3D12_BLEND_DEST_ALPHA);
    CHECK(d3d12::alphaBlend(BlendFactor::OneMinusDstColor) == D3D12_BLEND_INV_DEST_ALPHA);
    CHECK(d3d12::alphaBlend(BlendFactor::Constant) == D3D12_BLEND_BLEND_FACTOR);
    CHECK(d3d12::alphaBlend(BlendFactor::OneMinusConstant) == D3D12_BLEND_INV_BLEND_FACTOR);
    CHECK(d3d12::alphaBlend(BlendFactor::SrcAlphaSaturate) == D3D12_BLEND_SRC_ALPHA_SAT);
    CHECK(d3d12::colorBlend(BlendFactor::SrcColor) == D3D12_BLEND_SRC_COLOR);
    CHECK(d3d12::colorBlend(BlendFactor::OneMinusDstColor) == D3D12_BLEND_INV_DEST_COLOR);
    CHECK(d3d12::blendOp(BlendOp::ReverseSubtract) == D3D12_BLEND_OP_REV_SUBTRACT);
    CHECK(d3d12::blendOp(BlendOp::Max) == D3D12_BLEND_OP_MAX);

    const std::array<ColorTarget, 2> targets = {
        ColorTarget{ .format = Format::RGBA8Unorm },
        ColorTarget{ .format = Format::RGBA16Float,
                     .blend = { .enable = true,
                                .srcColor = BlendFactor::SrcColor,
                                .dstColor = BlendFactor::OneMinusSrcAlpha,
                                .colorOp = BlendOp::Subtract,
                                .srcAlpha = BlendFactor::SrcColor,
                                .dstAlpha = BlendFactor::OneMinusDstColor,
                                .alphaOp = BlendOp::Min } },
    };
    const D3D12_BLEND_DESC blend = d3d12::blendDesc(targets);
    CHECK(blend.RenderTarget[0].BlendEnable == FALSE);
    const D3D12_RENDER_TARGET_BLEND_DESC& blended = blend.RenderTarget[1];
    CHECK(blended.BlendEnable == TRUE);
    CHECK(blended.LogicOpEnable == FALSE);
    CHECK(blended.SrcBlend == D3D12_BLEND_SRC_COLOR);
    CHECK(blended.DestBlend == D3D12_BLEND_INV_SRC_ALPHA);
    CHECK(blended.BlendOp == D3D12_BLEND_OP_SUBTRACT);
    CHECK(blended.SrcBlendAlpha == D3D12_BLEND_SRC_ALPHA);
    CHECK(blended.DestBlendAlpha == D3D12_BLEND_INV_DEST_ALPHA);
    CHECK(blended.BlendOpAlpha == D3D12_BLEND_OP_MIN);
    // A render target past the colours keeps the default state.
    CHECK(blend.RenderTarget[2].SrcBlend == D3D12_BLEND_ONE);
    CHECK(blend.RenderTarget[2].DestBlend == D3D12_BLEND_ZERO);

    Device device = makeDevice();
    const ShaderLibrary library = loadStandard(device);
    GraphicsPipelineDesc desc = standardDesc(library);
    desc.colors = targets;
    create(device, desc);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a mesh pipeline with a task entry streams AS, MS and PS and no input layout; without one, MS and PS", "[d3d12]")
{
    Device device = makeDevice();
    ID3D12RootSignature* global = DeviceImpl::of(device).roots.global();
    const d3d12::PipelineShaders withTask{ .task = bytecodeOf(kTaskBytes), .mesh = bytecodeOf(kMeshBytes), .fragment = bytecodeOf(kFragmentBytes) };
    const d3d12::PipelineStream tasked = d3d12::graphicsStream(global, GraphicsPipelineDesc{ .colors = kColor }, withTask);
    CHECK(valueOf<D3D12_SHADER_BYTECODE>(tasked, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS).pShaderBytecode == kTaskBytes.data());
    CHECK(valueOf<D3D12_SHADER_BYTECODE>(tasked, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS).pShaderBytecode == kMeshBytes.data());
    CHECK(valueOf<D3D12_SHADER_BYTECODE>(tasked, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS).pShaderBytecode == kFragmentBytes.data());
    CHECK_FALSE(tasked.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT));
    CHECK_FALSE(tasked.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS));

    const d3d12::PipelineShaders withoutTask{ .mesh = bytecodeOf(kMeshBytes), .fragment = bytecodeOf(kFragmentBytes) };
    const d3d12::PipelineStream untasked = d3d12::graphicsStream(global, GraphicsPipelineDesc{ .colors = kColor }, withoutTask);
    CHECK_FALSE(untasked.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS));
    CHECK(untasked.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS));
    CHECK(untasked.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS));
    CHECK_FALSE(untasked.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT));

    const ShaderLibrary library = loadStandard(device);
    for (const bool task : { true, false })
    {
        GraphicsPipelineDesc desc{ .mesh = { library, "msMain" }, .fragment = { library, "fsMain" }, .colors = kColor, .label = "test.mesh" };
        if (task)
        {
            desc.task = { library, "tsMain" };
        }
        const d3d12::PipelineState& state = stateOf(device, create(device, desc));
        CHECK(state.kind == d3d12::PipelineKind::Mesh);
        CHECK(state.listTopology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a mesh stream whose DXIL declares no output topology names no topology type, and a vertex stream the type of its class", "[d3d12]")
{
    Device device = makeDevice();
    ID3D12RootSignature* global = DeviceImpl::of(device).roots.global();
    // A mesh pipeline ignores the class: its mesh entry declares its output topology, which these stand-in bytes do not.
    const d3d12::PipelineShaders meshShaders{ .mesh = bytecodeOf(kMeshBytes), .fragment = bytecodeOf(kFragmentBytes) };
    const d3d12::PipelineStream mesh = d3d12::graphicsStream(global, GraphicsPipelineDesc{ .primitive = PrimitiveClass::Line, .colors = kColor }, meshShaders);
    CHECK_FALSE(mesh.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY));
    const d3d12::PipelineStream lines =
        d3d12::graphicsStream(global, GraphicsPipelineDesc{ .primitive = PrimitiveClass::Line, .colors = kColor }, vertexShaders());
    CHECK(valueOf<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(lines, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY) == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a vertex pipeline without a fragment entry or a depth format is created", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    ID3D12RootSignature* global = DeviceImpl::of(device).roots.global();
    const d3d12::PipelineStream stream = d3d12::graphicsStream(global, GraphicsPipelineDesc{}, d3d12::PipelineShaders{ .vertex = bytecodeOf(kVertexBytes) });
    CHECK_FALSE(stream.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS));
    CHECK(valueOf<DXGI_FORMAT>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT) == DXGI_FORMAT_UNKNOWN);
    CHECK(valueOf<D3D12_RT_FORMAT_ARRAY>(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS).NumRenderTargets == 0);

    const ShaderLibrary vertex = load(device, "AbiVertex");
    const d3d12::PipelineState& state = stateOf(device, create(device, GraphicsPipelineDesc{ .vertex = { vertex, "vertexMain" }, .label = "test.abiVertex" }));
    CHECK(state.kind == d3d12::PipelineKind::Graphics);
    CHECK(state.listTopology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("createPipeline returns a PSO for each Abi library's entries and counts one creation each", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    core::DeviceCore& core = DeviceImpl::of(device).core;
    std::vector<ShaderLibrary> computeLibraries;
    for (const std::string_view name : { "AbiCompute", "AbiTextures", "AbiWriteOnly" })
    {
        computeLibraries.push_back(load(device, name));
    }
    const ShaderLibrary vertex = load(device, "AbiVertex");
    const ShaderLibrary fragment = load(device, "AbiFragment");
    const ShaderLibrary draw = load(device, "DrawOracle");
    const ShaderLibrary mesh = load(device, "AbiMesh");
    const Counters before = device.counters();

    std::vector<std::pair<Pipeline, d3d12::PipelineKind>> created;
    for (const ShaderLibrary library : computeLibraries)
    {
        std::vector<std::string> entries;
        core.pool<ShaderLibrary>().update(library, [&](const core::ShaderLibraryRecord& record) {
            for (const core::ShaderEntryRecord& entry : record.entries)
            {
                entries.push_back(entry.name);
            }
        });
        CHECK_FALSE(entries.empty());
        for (const std::string& entry : entries)
        {
            created.emplace_back(createCompute(device, library, entry), d3d12::PipelineKind::Compute);
        }
    }
    created.emplace_back(
        create(device, GraphicsPipelineDesc{ .vertex = { vertex, "vertexMain" }, .colors = kColor, .label = "test.abiVertex" }), d3d12::PipelineKind::Graphics
    );
    const std::array<ColorTarget, 1> linear = { ColorTarget{ .format = Format::RGBA16Float } };
    created.emplace_back(
        create(
            device,
            GraphicsPipelineDesc{
                .vertex = { draw, "oracleVertexMain" }, .fragment = { fragment, "fragmentMain" }, .colors = linear, .label = "test.abiFragment" }
        ),
        d3d12::PipelineKind::Graphics
    );
    created.emplace_back(
        create(device, GraphicsPipelineDesc{ .task = { mesh, "taskMain" }, .mesh = { mesh, "meshMain" }, .colors = kColor, .label = "test.abiMesh" }),
        d3d12::PipelineKind::Mesh
    );

    const Counters after = device.counters();
    CHECK(after.pipelinesCreated == before.pipelinesCreated + created.size());
    CHECK(after.nativeCreateCalls == before.nativeCreateCalls + created.size());
    CHECK(after.pipelineCacheMisses == before.pipelineCacheMisses + created.size());
    CHECK(after.pipelineCacheHits == before.pipelineCacheHits);
    CHECK(after.pipelineCompileNanoseconds > before.pipelineCompileNanoseconds);
    for (const auto& [pipeline, kind] : created)
    {
        const d3d12::PipelineState& state = stateOf(device, pipeline);
        CHECK(state.kind == kind);
        CHECK(state.listTopology == (kind == d3d12::PipelineKind::Graphics ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D_PRIMITIVE_TOPOLOGY_UNDEFINED));
        CHECK_FALSE(state.depthBias);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a destroyed pipeline's PSO is released when its frame retires", "[d3d12]")
{
    Device device = makeDevice();
    core::DeviceCore& core = DeviceImpl::of(device).core;
    const ShaderLibrary library = loadStandard(device);
    const size_t before = core.pool<Pipeline>().liveCount();
    const Pipeline pipeline = create(device, standardDesc(library));
    ID3D12PipelineState* pso = stateOf(device, pipeline).pso;
    const ULONG held = pso->AddRef();
    CHECK(core.pool<Pipeline>().liveCount() == before + 1);
    const uint64_t retired = device.counters().deferredDestructionsRetired;

    device.destroy(pipeline);
    for (int frame = 0; frame < 3; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    CHECK(core.pool<Pipeline>().liveCount() == before);
    CHECK(device.counters().deferredDestructionsRetired == retired + 1);
    // The payload's reference is gone: releasing the one this case took leaves one fewer than before it.
    CHECK(pso->Release() == held - 2);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a PSO creation the runtime refuses is DriverFailure naming the label", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    Device device = makeDevice();
    if (DeviceImpl::of(device).loaded.layer)
    {
        destroyDevice(device);
        SKIP("the debug layer reports the refusal as an error message, which aborts by design");
    }
    core::DeviceCore& core = DeviceImpl::of(device).core;
    const ShaderLibrary standard = loadStandard(device);
    const ShaderLibrary fragment = load(device, "AbiFragment");
    const size_t before = core.pool<Pipeline>().liveCount();
    const uint64_t createdBefore = device.counters().pipelinesCreated;
    // The fragment entry reads varyings the vertex entry does not write, which the core cannot see and D3D12 refuses.
    const Result<Pipeline> pipeline = device.createPipeline(
        GraphicsPipelineDesc{ .vertex = { standard, "vsMain" }, .fragment = { fragment, "fragmentMain" }, .colors = kColor, .label = "test.mismatch" }
    );
    REQUIRE_FALSE(pipeline.has_value());
    CHECK(pipeline.error().category == ErrorCategory::DriverFailure);
    CHECK_THAT(pipeline.error().message, Catch::Matchers::ContainsSubstring("\"test.mismatch\""));
    CHECK_THAT(pipeline.error().message, Catch::Matchers::ContainsSubstring("CreatePipelineState"));
    CHECK(core.pool<Pipeline>().liveCount() == before);
    CHECK(device.counters().pipelinesCreated == createdBefore);
    destroyDevice(device);
}
