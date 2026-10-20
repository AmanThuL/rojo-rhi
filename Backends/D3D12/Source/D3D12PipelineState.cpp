//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12PipelineState.cpp
/// @brief Implements the D3D12 pipeline state conversions, the pipeline state streams and the pipeline retire hook.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12PipelineState.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "D3D12Common.h"
#include "D3D12Formats.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace rojoRHI::d3d12 {
    namespace {
        //==============================================================================================================
        /// Returns `value` rounded up to a multiple of `alignment`, a power of two.
        size_t alignUp(size_t value, size_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        //==============================================================================================================
        /// Returns the number of `colors` a pipeline streams: all of them, at most the render targets D3D12 has.
        size_t targetCount(std::span<const ColorTarget> colors)
        {
            return std::min<size_t>(colors.size(), D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
        }

        //==============================================================================================================
        /// Returns the stencil operations of one face, with the masks both faces share.
        D3D12_DEPTH_STENCILOP_DESC1 stencilFace(const StencilFaceState& face, uint8_t readBits, uint8_t writeBits)
        {
            D3D12_DEPTH_STENCILOP_DESC1 native = {};
            native.StencilFailOp = stencilOp(face.fail);
            native.StencilDepthFailOp = stencilOp(face.depthFail);
            native.StencilPassOp = stencilOp(face.pass);
            native.StencilFunc = comparisonFunc(face.compare);
            native.StencilReadMask = readBits;
            native.StencilWriteMask = writeBits;
            return native;
        }
    } // namespace

    //==================================================================================================================
    D3D12_BLEND colorBlend(BlendFactor factor)
    {
        switch (factor)
        {
        case BlendFactor::Zero: return D3D12_BLEND_ZERO;
        case BlendFactor::One: return D3D12_BLEND_ONE;
        case BlendFactor::SrcColor: return D3D12_BLEND_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
        case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DstColor: return D3D12_BLEND_DEST_COLOR;
        case BlendFactor::OneMinusDstColor: return D3D12_BLEND_INV_DEST_COLOR;
        case BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::SrcAlphaSaturate: return D3D12_BLEND_SRC_ALPHA_SAT;
        case BlendFactor::Constant: return D3D12_BLEND_BLEND_FACTOR;
        case BlendFactor::OneMinusConstant: return D3D12_BLEND_INV_BLEND_FACTOR;
        }
        return D3D12_BLEND_ZERO;
    }

    //==================================================================================================================
    D3D12_BLEND alphaBlend(BlendFactor factor)
    {
        switch (factor)
        {
        case BlendFactor::Zero: return D3D12_BLEND_ZERO;
        case BlendFactor::One: return D3D12_BLEND_ONE;
        case BlendFactor::SrcColor: return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrcColor: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DstColor: return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDstColor: return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::SrcAlphaSaturate: return D3D12_BLEND_SRC_ALPHA_SAT;
        case BlendFactor::Constant: return D3D12_BLEND_BLEND_FACTOR;
        case BlendFactor::OneMinusConstant: return D3D12_BLEND_INV_BLEND_FACTOR;
        }
        return D3D12_BLEND_ZERO;
    }

    //==================================================================================================================
    D3D12_BLEND_OP blendOp(BlendOp op)
    {
        switch (op)
        {
        case BlendOp::Add: return D3D12_BLEND_OP_ADD;
        case BlendOp::Subtract: return D3D12_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
        case BlendOp::Min: return D3D12_BLEND_OP_MIN;
        case BlendOp::Max: return D3D12_BLEND_OP_MAX;
        }
        return D3D12_BLEND_OP_ADD;
    }

    //==================================================================================================================
    UINT8 writeMask(ColorWriteMask mask)
    {
        UINT bits = 0;
        if ((mask & ColorWriteMask::R) != ColorWriteMask::None)
        {
            bits |= D3D12_COLOR_WRITE_ENABLE_RED;
        }
        if ((mask & ColorWriteMask::G) != ColorWriteMask::None)
        {
            bits |= D3D12_COLOR_WRITE_ENABLE_GREEN;
        }
        if ((mask & ColorWriteMask::B) != ColorWriteMask::None)
        {
            bits |= D3D12_COLOR_WRITE_ENABLE_BLUE;
        }
        if ((mask & ColorWriteMask::A) != ColorWriteMask::None)
        {
            bits |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
        }
        return static_cast<UINT8>(bits);
    }

    //==================================================================================================================
    D3D12_CULL_MODE cullMode(CullMode cull)
    {
        switch (cull)
        {
        case CullMode::None: return D3D12_CULL_MODE_NONE;
        case CullMode::Front: return D3D12_CULL_MODE_FRONT;
        case CullMode::Back: return D3D12_CULL_MODE_BACK;
        }
        return D3D12_CULL_MODE_NONE;
    }

    //==================================================================================================================
    D3D12_FILL_MODE fillMode(FillMode fill)
    {
        switch (fill)
        {
        case FillMode::Solid: return D3D12_FILL_MODE_SOLID;
        case FillMode::Wireframe: return D3D12_FILL_MODE_WIREFRAME;
        }
        return D3D12_FILL_MODE_SOLID;
    }

    //==================================================================================================================
    D3D12_COMPARISON_FUNC comparisonFunc(CompareOp compare)
    {
        switch (compare)
        {
        case CompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
        case CompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
        case CompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
        case CompareOp::LessOrEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
        case CompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case CompareOp::GreaterOrEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case CompareOp::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
        }
        return D3D12_COMPARISON_FUNC_ALWAYS;
    }

    //==================================================================================================================
    D3D12_STENCIL_OP stencilOp(StencilOp op)
    {
        switch (op)
        {
        case StencilOp::Keep: return D3D12_STENCIL_OP_KEEP;
        case StencilOp::Zero: return D3D12_STENCIL_OP_ZERO;
        case StencilOp::Replace: return D3D12_STENCIL_OP_REPLACE;
        case StencilOp::IncrementClamp: return D3D12_STENCIL_OP_INCR_SAT;
        case StencilOp::DecrementClamp: return D3D12_STENCIL_OP_DECR_SAT;
        case StencilOp::Invert: return D3D12_STENCIL_OP_INVERT;
        case StencilOp::IncrementWrap: return D3D12_STENCIL_OP_INCR;
        case StencilOp::DecrementWrap: return D3D12_STENCIL_OP_DECR;
        }
        return D3D12_STENCIL_OP_KEEP;
    }

    //==================================================================================================================
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType(PrimitiveClass primitive)
    {
        switch (primitive)
        {
        case PrimitiveClass::Point: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case PrimitiveClass::Line: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case PrimitiveClass::Triangle: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        }
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }

    //==================================================================================================================
    D3D_PRIMITIVE_TOPOLOGY listTopology(PrimitiveClass primitive)
    {
        switch (primitive)
        {
        case PrimitiveClass::Point: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case PrimitiveClass::Line: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrimitiveClass::Triangle: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }

    //==================================================================================================================
    D3D12_BLEND_DESC blendDesc(std::span<const ColorTarget> colors)
    {
        D3D12_BLEND_DESC native = {};
        native.AlphaToCoverageEnable = FALSE;
        native.IndependentBlendEnable = TRUE;
        for (D3D12_RENDER_TARGET_BLEND_DESC& target : native.RenderTarget)
        {
            target.BlendEnable = FALSE;
            target.LogicOpEnable = FALSE;
            target.SrcBlend = D3D12_BLEND_ONE;
            target.DestBlend = D3D12_BLEND_ZERO;
            target.BlendOp = D3D12_BLEND_OP_ADD;
            target.SrcBlendAlpha = D3D12_BLEND_ONE;
            target.DestBlendAlpha = D3D12_BLEND_ZERO;
            target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            target.LogicOp = D3D12_LOGIC_OP_NOOP;
            target.RenderTargetWriteMask = static_cast<UINT8>(D3D12_COLOR_WRITE_ENABLE_ALL);
        }
        for (size_t i = 0; i < targetCount(colors); ++i)
        {
            const ColorTarget& color = colors[i];
            D3D12_RENDER_TARGET_BLEND_DESC& target = native.RenderTarget[i];
            target.BlendEnable = color.blend.enable ? TRUE : FALSE;
            target.SrcBlend = colorBlend(color.blend.srcColor);
            target.DestBlend = colorBlend(color.blend.dstColor);
            target.BlendOp = blendOp(color.blend.colorOp);
            target.SrcBlendAlpha = alphaBlend(color.blend.srcAlpha);
            target.DestBlendAlpha = alphaBlend(color.blend.dstAlpha);
            target.BlendOpAlpha = blendOp(color.blend.alphaOp);
            target.RenderTargetWriteMask = writeMask(color.writeMask);
        }
        return native;
    }

    //==================================================================================================================
    D3D12_RASTERIZER_DESC2 rasterizerDesc(const RasterState& raster)
    {
        D3D12_RASTERIZER_DESC2 native = {};
        native.FillMode = fillMode(raster.fill);
        native.CullMode = cullMode(raster.cull);
        native.FrontCounterClockwise = raster.frontFace == FrontFace::CounterClockwise ? TRUE : FALSE;
        native.DepthBias = 0.0f;
        native.DepthBiasClamp = 0.0f;
        native.SlopeScaledDepthBias = 0.0f;
        native.DepthClipEnable = raster.depthClip ? TRUE : FALSE;
        native.LineRasterizationMode = D3D12_LINE_RASTERIZATION_MODE_ALIASED;
        native.ForcedSampleCount = 0;
        native.ConservativeRaster = raster.conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        return native;
    }

    //==================================================================================================================
    D3D12_DEPTH_STENCIL_DESC2 depthStencilDesc(const DepthStencilState& state)
    {
        D3D12_DEPTH_STENCIL_DESC2 native = {};
        native.DepthEnable = state.depthTest ? TRUE : FALSE;
        native.DepthWriteMask = state.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        native.DepthFunc = comparisonFunc(state.depthCompare);
        native.StencilEnable = state.stencilTest ? TRUE : FALSE;
        native.FrontFace = stencilFace(state.front, state.stencilReadMask, state.stencilWriteMask);
        native.BackFace = stencilFace(state.back, state.stencilReadMask, state.stencilWriteMask);
        native.DepthBoundsTestEnable = FALSE;
        return native;
    }

    //==================================================================================================================
    D3D12_RT_FORMAT_ARRAY renderTargetFormats(std::span<const ColorTarget> colors)
    {
        D3D12_RT_FORMAT_ARRAY native = {};
        const size_t count = targetCount(colors);
        for (size_t i = 0; i < count; ++i)
        {
            native.RTFormats[i] = dxgiFormat(colors[i].format);
        }
        native.NumRenderTargets = static_cast<UINT>(count);
        return native;
    }

    //==================================================================================================================
    bool PipelineStream::has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) const
    {
        return locate(type) != nullptr;
    }

    //==================================================================================================================
    D3D12_PIPELINE_STATE_STREAM_DESC PipelineStream::desc() const
    {
        D3D12_PIPELINE_STATE_STREAM_DESC native = {};
        native.SizeInBytes = m_size;
        // The runtime reads the stream and never writes it; the field is not const-qualified.
        native.pPipelineStateSubobjectStream = const_cast<uint64_t*>(m_words.data());
        return native;
    }

    //==================================================================================================================
    std::byte* PipelineStream::place(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, size_t alignment, size_t size)
    {
        // `m_size` is a multiple of the pointer size, so the type starts a subobject where the runtime looks for one.
        const size_t valueAt = alignUp(m_size + sizeof(type), alignment);
        const size_t end = alignUp(valueAt + size, sizeof(void*));
        ROJORHI_INTERNAL_ASSERT(end <= sizeof(m_words) && m_count < kMaxSubobjects, "a pipeline state stream outgrew its storage");
        auto* bytes = reinterpret_cast<std::byte*>(m_words.data());
        std::memcpy(bytes + m_size, &type, sizeof(type));
        m_placed[m_count] = Placed{ type, valueAt };
        ++m_count;
        m_size = end;
        return bytes + valueAt;
    }

    //==================================================================================================================
    const std::byte* PipelineStream::locate(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) const
    {
        const auto* bytes = reinterpret_cast<const std::byte*>(m_words.data());
        for (size_t i = 0; i < m_count; ++i)
        {
            if (m_placed[i].type == type)
            {
                return bytes + m_placed[i].offset;
            }
        }
        return nullptr;
    }

    //==================================================================================================================
    PipelineStream graphicsStream(ID3D12RootSignature* root, const GraphicsPipelineDesc& desc, const PipelineShaders& shaders)
    {
        PipelineStream stream;
        stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root);
        if (shaders.mesh.pShaderBytecode != nullptr)
        {
            // A mesh pipeline takes no input layout.
            if (shaders.task.pShaderBytecode != nullptr)
            {
                stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, shaders.task);
            }
            stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, shaders.mesh);
        }
        else
        {
            // Vertices are pulled by the shader, so the input layout is empty.
            stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, shaders.vertex);
            stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC{ nullptr, 0 });
        }
        if (shaders.fragment.pShaderBytecode != nullptr)
        {
            stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, shaders.fragment);
        }
        stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blendDesc(desc.colors));
        stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, std::numeric_limits<UINT>::max());
        stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2, rasterizerDesc(desc.raster));
        stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2, depthStencilDesc(desc.depthStencil));
        if (shaders.mesh.pShaderBytecode == nullptr)
        {
            // A mesh pipeline ignores the class: its mesh entry declares its output topology.
            stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, topologyType(desc.primitive));
        }
        stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, renderTargetFormats(desc.colors));
        stream.add(
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, desc.depthFormat == Format::Undefined ? DXGI_FORMAT_UNKNOWN : dsvFormat(desc.depthFormat)
        );
        stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC{ desc.samples, 0 });
        stream.add(
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, desc.raster.depthBias ? D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS : D3D12_PIPELINE_STATE_FLAG_NONE
        );
        return stream;
    }

    //==================================================================================================================
    PipelineStream computeStream(ID3D12RootSignature* root, D3D12_SHADER_BYTECODE bytecode)
    {
        PipelineStream stream;
        stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root);
        stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, bytecode);
        return stream;
    }

    //==================================================================================================================
    void retirePipeline(void*, core::PipelineRecord& record)
    {
        auto* state = static_cast<PipelineState*>(record.native);
        if (state == nullptr)
        {
            return;
        }
        state->pso->Release();
        delete state;
        record.native = nullptr;
    }
} // namespace rojoRHI::d3d12
