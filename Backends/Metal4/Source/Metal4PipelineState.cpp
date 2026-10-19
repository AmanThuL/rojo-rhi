//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4PipelineState.cpp
/// @brief Implements the pipeline payload, the enum conversions and the depth-stencil cache of the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4PipelineState.h"

#include "Metal4Common.h"
#include "Metal4Formats.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

namespace rojoRHI::metal4 {
    namespace {
        //==============================================================================================================
        /// Packs a face's stencil operations and comparison into 12 bits.
        uint64_t packFace(const StencilFaceState& face)
        {
            return uint64_t{ static_cast<uint8_t>(face.fail) } | uint64_t{ static_cast<uint8_t>(face.depthFail) } << 3 |
                   uint64_t{ static_cast<uint8_t>(face.pass) } << 6 | uint64_t{ static_cast<uint8_t>(face.compare) } << 9;
        }

        //==============================================================================================================
        /// Packs what the descriptor of `state` receives: the depth comparison (`Always` when depth is untested), the depth
        /// write and, when the stencil test is on, the masks and both faces. Two states with one key make equal objects.
        uint64_t keyOf(const DepthStencilState& state)
        {
            const CompareOp depthCompare = state.depthTest ? state.depthCompare : CompareOp::Always;
            uint64_t key = uint64_t{ static_cast<uint8_t>(depthCompare) } | uint64_t{ state.depthWrite } << 3;
            if (state.stencilTest)
            {
                key |= uint64_t{ 1 } << 4 | uint64_t{ state.stencilReadMask } << 5 | uint64_t{ state.stencilWriteMask } << 13 | packFace(state.front) << 21 |
                       packFace(state.back) << 33;
            }
            return key;
        }

        //==============================================================================================================
        /// Returns a new stencil descriptor (+1) for `face` with the masks of `state`.
        MTL::StencilDescriptor* newStencil(const StencilFaceState& face, const DepthStencilState& state)
        {
            MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
            stencil->setStencilFailureOperation(stencilOperation(face.fail));
            stencil->setDepthFailureOperation(stencilOperation(face.depthFail));
            stencil->setDepthStencilPassOperation(stencilOperation(face.pass));
            stencil->setStencilCompareFunction(compareFunction(face.compare));
            stencil->setReadMask(state.stencilReadMask);
            stencil->setWriteMask(state.stencilWriteMask);
            return stencil;
        }
    } // namespace

    //==================================================================================================================
    void PipelineState::release()
    {
        if (render != nullptr)
        {
            render->release();
            render = nullptr;
        }
        if (compute != nullptr)
        {
            compute->release();
            compute = nullptr;
        }
        depthStencil = nullptr;
    }

    //==================================================================================================================
    MTL::BlendFactor blendFactor(BlendFactor factor)
    {
        switch (factor)
        {
        case BlendFactor::Zero: return MTL::BlendFactorZero;
        case BlendFactor::One: return MTL::BlendFactorOne;
        case BlendFactor::SrcColor: return MTL::BlendFactorSourceColor;
        case BlendFactor::OneMinusSrcColor: return MTL::BlendFactorOneMinusSourceColor;
        case BlendFactor::SrcAlpha: return MTL::BlendFactorSourceAlpha;
        case BlendFactor::OneMinusSrcAlpha: return MTL::BlendFactorOneMinusSourceAlpha;
        case BlendFactor::DstColor: return MTL::BlendFactorDestinationColor;
        case BlendFactor::OneMinusDstColor: return MTL::BlendFactorOneMinusDestinationColor;
        case BlendFactor::DstAlpha: return MTL::BlendFactorDestinationAlpha;
        case BlendFactor::OneMinusDstAlpha: return MTL::BlendFactorOneMinusDestinationAlpha;
        case BlendFactor::SrcAlphaSaturate: return MTL::BlendFactorSourceAlphaSaturated;
        case BlendFactor::Constant: return MTL::BlendFactorBlendColor;
        case BlendFactor::OneMinusConstant: return MTL::BlendFactorOneMinusBlendColor;
        }
        return MTL::BlendFactorZero;
    }

    //==================================================================================================================
    MTL::BlendOperation blendOperation(BlendOp op)
    {
        switch (op)
        {
        case BlendOp::Add: return MTL::BlendOperationAdd;
        case BlendOp::Subtract: return MTL::BlendOperationSubtract;
        case BlendOp::ReverseSubtract: return MTL::BlendOperationReverseSubtract;
        case BlendOp::Min: return MTL::BlendOperationMin;
        case BlendOp::Max: return MTL::BlendOperationMax;
        }
        return MTL::BlendOperationAdd;
    }

    //==================================================================================================================
    // RojoRHI numbers the channels R = 1 to A = 8; Metal numbers them A = 1 to R = 8, so each bit moves on its own.
    MTL::ColorWriteMask colorWriteMask(ColorWriteMask mask)
    {
        const auto has = [mask](ColorWriteMask channel) { return (mask & channel) == channel; };
        NS::UInteger bits = MTL::ColorWriteMaskNone;
        bits |= has(ColorWriteMask::R) ? MTL::ColorWriteMaskRed : 0;
        bits |= has(ColorWriteMask::G) ? MTL::ColorWriteMaskGreen : 0;
        bits |= has(ColorWriteMask::B) ? MTL::ColorWriteMaskBlue : 0;
        bits |= has(ColorWriteMask::A) ? MTL::ColorWriteMaskAlpha : 0;
        return static_cast<MTL::ColorWriteMask>(bits);
    }

    //==================================================================================================================
    MTL::CompareFunction compareFunction(CompareOp compare)
    {
        switch (compare)
        {
        case CompareOp::Never: return MTL::CompareFunctionNever;
        case CompareOp::Less: return MTL::CompareFunctionLess;
        case CompareOp::Equal: return MTL::CompareFunctionEqual;
        case CompareOp::LessOrEqual: return MTL::CompareFunctionLessEqual;
        case CompareOp::Greater: return MTL::CompareFunctionGreater;
        case CompareOp::NotEqual: return MTL::CompareFunctionNotEqual;
        case CompareOp::GreaterOrEqual: return MTL::CompareFunctionGreaterEqual;
        case CompareOp::Always: return MTL::CompareFunctionAlways;
        }
        return MTL::CompareFunctionAlways;
    }

    //==================================================================================================================
    MTL::StencilOperation stencilOperation(StencilOp op)
    {
        switch (op)
        {
        case StencilOp::Keep: return MTL::StencilOperationKeep;
        case StencilOp::Zero: return MTL::StencilOperationZero;
        case StencilOp::Replace: return MTL::StencilOperationReplace;
        case StencilOp::IncrementClamp: return MTL::StencilOperationIncrementClamp;
        case StencilOp::DecrementClamp: return MTL::StencilOperationDecrementClamp;
        case StencilOp::Invert: return MTL::StencilOperationInvert;
        case StencilOp::IncrementWrap: return MTL::StencilOperationIncrementWrap;
        case StencilOp::DecrementWrap: return MTL::StencilOperationDecrementWrap;
        }
        return MTL::StencilOperationKeep;
    }

    //==================================================================================================================
    MTL::CullMode cullMode(CullMode cull)
    {
        switch (cull)
        {
        case CullMode::None: return MTL::CullModeNone;
        case CullMode::Front: return MTL::CullModeFront;
        case CullMode::Back: return MTL::CullModeBack;
        }
        return MTL::CullModeNone;
    }

    //==================================================================================================================
    MTL::Winding winding(FrontFace face)
    {
        switch (face)
        {
        case FrontFace::CounterClockwise: return MTL::WindingCounterClockwise;
        case FrontFace::Clockwise: return MTL::WindingClockwise;
        }
        return MTL::WindingCounterClockwise;
    }

    //==================================================================================================================
    MTL::TriangleFillMode triangleFillMode(FillMode fill)
    {
        switch (fill)
        {
        case FillMode::Solid: return MTL::TriangleFillModeFill;
        case FillMode::Wireframe: return MTL::TriangleFillModeLines;
        }
        return MTL::TriangleFillModeFill;
    }

    //==================================================================================================================
    MTL::PrimitiveTopologyClass topologyClass(PrimitiveClass primitive)
    {
        switch (primitive)
        {
        case PrimitiveClass::Point: return MTL::PrimitiveTopologyClassPoint;
        case PrimitiveClass::Line: return MTL::PrimitiveTopologyClassLine;
        case PrimitiveClass::Triangle: return MTL::PrimitiveTopologyClassTriangle;
        }
        return MTL::PrimitiveTopologyClassTriangle;
    }

    //==================================================================================================================
    MTL::PrimitiveType listType(PrimitiveClass primitive)
    {
        switch (primitive)
        {
        case PrimitiveClass::Point: return MTL::PrimitiveTypePoint;
        case PrimitiveClass::Line: return MTL::PrimitiveTypeLine;
        case PrimitiveClass::Triangle: return MTL::PrimitiveTypeTriangle;
        }
        return MTL::PrimitiveTypeTriangle;
    }

    //==================================================================================================================
    MTL::PrimitiveType primitiveType(PrimitiveTopology topology)
    {
        switch (topology)
        {
        case PrimitiveTopology::PointList: return MTL::PrimitiveTypePoint;
        case PrimitiveTopology::LineList: return MTL::PrimitiveTypeLine;
        case PrimitiveTopology::LineStrip: return MTL::PrimitiveTypeLineStrip;
        case PrimitiveTopology::TriangleList: return MTL::PrimitiveTypeTriangle;
        case PrimitiveTopology::TriangleStrip: return MTL::PrimitiveTypeTriangleStrip;
        }
        return MTL::PrimitiveTypeTriangle;
    }

    //==================================================================================================================
    MTL::IndexType indexType(IndexType type)
    {
        switch (type)
        {
        case IndexType::UInt16: return MTL::IndexTypeUInt16;
        case IndexType::UInt32: return MTL::IndexTypeUInt32;
        }
        return MTL::IndexTypeUInt32;
    }

    //==================================================================================================================
    void describeColors(MTL4::RenderPipelineColorAttachmentDescriptorArray* attachments, std::span<const ColorTarget> colors)
    {
        for (size_t i = 0; i < colors.size(); ++i)
        {
            const ColorTarget& target = colors[i];
            MTL4::RenderPipelineColorAttachmentDescriptor* attachment = attachments->object(i);
            attachment->setPixelFormat(pixelFormat(target.format));
            attachment->setWriteMask(colorWriteMask(target.writeMask));
            attachment->setBlendingState(target.blend.enable ? MTL4::BlendStateEnabled : MTL4::BlendStateDisabled);
            if (target.blend.enable)
            {
                attachment->setSourceRGBBlendFactor(blendFactor(target.blend.srcColor));
                attachment->setDestinationRGBBlendFactor(blendFactor(target.blend.dstColor));
                attachment->setRgbBlendOperation(blendOperation(target.blend.colorOp));
                attachment->setSourceAlphaBlendFactor(blendFactor(target.blend.srcAlpha));
                attachment->setDestinationAlphaBlendFactor(blendFactor(target.blend.dstAlpha));
                attachment->setAlphaBlendOperation(blendOperation(target.blend.alphaOp));
            }
        }
    }

    //==================================================================================================================
    bool testsDepthOrStencil(const DepthStencilState& state)
    {
        return state.depthTest || state.stencilTest;
    }

    //==================================================================================================================
    MTL::DepthStencilState* DepthStencilCache::get(MTL::Device* device, const DepthStencilState& state, core::DeviceCounts& counts)
    {
        if (!testsDepthOrStencil(state))
        {
            return nullptr;
        }
        const uint64_t key = keyOf(state);
        std::lock_guard lock(m_mutex);
        if (const auto found = m_states.find(key); found != m_states.end())
        {
            return found->second;
        }
        MTL::DepthStencilDescriptor* descriptor = MTL::DepthStencilDescriptor::alloc()->init();
        descriptor->setDepthCompareFunction(state.depthTest ? compareFunction(state.depthCompare) : MTL::CompareFunctionAlways);
        descriptor->setDepthWriteEnabled(state.depthWrite);
        if (state.stencilTest)
        {
            MTL::StencilDescriptor* front = newStencil(state.front, state);
            MTL::StencilDescriptor* back = newStencil(state.back, state);
            descriptor->setFrontFaceStencil(front);
            descriptor->setBackFaceStencil(back);
            back->release();
            front->release();
        }
        descriptor->setLabel(makeString("rojorhi.depthStencil"));
        MTL::DepthStencilState* created = device->newDepthStencilState(descriptor);
        core::DeviceCounts::add(counts.nativeCreateCalls);
        descriptor->release();
        if (created != nullptr)
        {
            m_states.emplace(key, created);
        }
        return created;
    }

    //==================================================================================================================
    size_t DepthStencilCache::size()
    {
        std::lock_guard lock(m_mutex);
        return m_states.size();
    }

    //==================================================================================================================
    void DepthStencilCache::release(core::DeviceCounts& counts)
    {
        std::lock_guard lock(m_mutex);
        for (const auto& [key, state] : m_states)
        {
            state->release();
        }
        core::DeviceCounts::add(counts.nativeCreateCalls, m_states.size());
        m_states.clear();
    }

    //==================================================================================================================
    void retirePipeline([[maybe_unused]] void* context, core::PipelineRecord& record)
    {
        const AutoreleasePool pool;
        auto* state = static_cast<PipelineState*>(record.native);
        if (state != nullptr)
        {
            state->release();
            delete state;
            record.native = nullptr;
        }
    }
} // namespace rojoRHI::metal4
