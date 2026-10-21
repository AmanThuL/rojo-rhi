//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4GraphicsState.cpp
/// @brief Implements the pipeline binding and dynamic-state entries for the Metal 4 backend: the prefix, the filtered encoder setters and compute binding.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Core/RenderPasses.h"
#include "Metal4DrawState.h"
#include "Metal4Encoders.h"
#include "Metal4ListState.h"
#include "Metal4PipelineState.h"

#include <Metal/Metal.hpp>

#include <array>
#include <cstdint>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Returns the open render encoder of `list`; one must be open.
        MTL4::RenderCommandEncoder* openEncoder(CommandListImpl& list)
        {
            MTL4::RenderCommandEncoder* encoder = list.encoders.renderEncoder();
            ROJORHI_INTERNAL_ASSERT(encoder != nullptr, "graphics state is applied with no render encoder open");
            return encoder;
        }

        //==============================================================================================================
        /// Sets the render pipeline state of `pipeline` on `encoder`, the open render encoder, unless it holds it. A mesh
        /// pipeline's object and mesh entries read the reserved bindings through the list's table, which the encoder binds to
        /// those stages at its first mesh pipeline: a stage the table is bound to costs every draw that follows a table write,
        /// so an encoder that draws no mesh binds only the vertex and fragment stages.
        void setRenderState(CommandListImpl& list, MTL4::RenderCommandEncoder* encoder, const metal4::PipelineState* pipeline)
        {
            metal4::RenderHeld& held = list.draw.held;
            if (held.pipeline != nullptr && held.pipeline->render == pipeline->render)
            {
                held.pipeline = pipeline;
                return;
            }
            if (pipeline->mesh && !held.meshStages)
            {
                encoder->setArgumentTable(list.state.table, MTL::RenderStageObject | MTL::RenderStageMesh);
                core::noteNativeCall(list.core);
                held.meshStages = true;
            }
            encoder->setRenderPipelineState(pipeline->render);
            core::noteNativeCall(list.core);
            held.pipeline = pipeline;
        }

        //==============================================================================================================
        /// Sets `state` on `encoder` unless it holds it; equal states share one object, so a pointer compares them. It sends
        /// `bound`, which is `state` or, for a null `state`, the device's untested state that equals Metal's default.
        void setDepthStencil(
            CommandListImpl& list, MTL4::RenderCommandEncoder* encoder, const MTL::DepthStencilState* state, const MTL::DepthStencilState* bound
        )
        {
            metal4::RenderHeld& held = list.draw.held;
            if (held.depthStencil == state)
            {
                return;
            }
            encoder->setDepthStencilState(bound);
            core::noteNativeCall(list.core);
            held.depthStencil = state;
        }

        //==============================================================================================================
        /// Sets `cull` on `encoder` unless it holds it.
        void setCull(CommandListImpl& list, MTL4::RenderCommandEncoder* encoder, MTL::CullMode cull)
        {
            metal4::RenderHeld& held = list.draw.held;
            if (held.cull == cull)
            {
                return;
            }
            encoder->setCullMode(cull);
            core::noteNativeCall(list.core);
            held.cull = cull;
        }

        //==============================================================================================================
        /// Sets `winding` on `encoder` unless it holds it.
        void setWinding(CommandListImpl& list, MTL4::RenderCommandEncoder* encoder, MTL::Winding winding)
        {
            metal4::RenderHeld& held = list.draw.held;
            if (held.winding == winding)
            {
                return;
            }
            encoder->setFrontFacingWinding(winding);
            core::noteNativeCall(list.core);
            held.winding = winding;
        }

        //==============================================================================================================
        /// Sets `fill` on `encoder` unless it holds it.
        void setFill(CommandListImpl& list, MTL4::RenderCommandEncoder* encoder, MTL::TriangleFillMode fill)
        {
            metal4::RenderHeld& held = list.draw.held;
            if (held.fill == fill)
            {
                return;
            }
            encoder->setTriangleFillMode(fill);
            core::noteNativeCall(list.core);
            held.fill = fill;
        }

        //==============================================================================================================
        /// Sets `clip` on `encoder` unless it holds it.
        void setDepthClip(CommandListImpl& list, MTL4::RenderCommandEncoder* encoder, MTL::DepthClipMode clip)
        {
            metal4::RenderHeld& held = list.draw.held;
            if (held.depthClip == clip)
            {
                return;
            }
            encoder->setDepthClipMode(clip);
            core::noteNativeCall(list.core);
            held.depthClip = clip;
        }

        //==============================================================================================================
        /// True when `a` and `b` are the same viewport.
        bool sameViewport(const MTL::Viewport& a, const MTL::Viewport& b)
        {
            return a.originX == b.originX && a.originY == b.originY && a.width == b.width && a.height == b.height && a.znear == b.znear && a.zfar == b.zfar;
        }

        //==============================================================================================================
        /// Sets `viewport` unless the encoder holds it.
        void applyViewport(CommandListImpl& list, const MTL::Viewport& viewport)
        {
            metal4::RenderHeld& held = list.draw.held;
            if (held.viewportKnown && sameViewport(held.viewport, viewport))
            {
                return;
            }
            openEncoder(list)->setViewport(viewport);
            core::noteNativeCall(list.core);
            held.viewportKnown = true;
            held.viewport = viewport;
        }

        //==============================================================================================================
        /// Sets `scissor` unless the encoder holds it.
        void applyScissor(CommandListImpl& list, const MTL::ScissorRect& scissor)
        {
            metal4::RenderHeld& held = list.draw.held;
            if (held.scissorKnown && held.scissor.x == scissor.x && held.scissor.y == scissor.y && held.scissor.width == scissor.width &&
                held.scissor.height == scissor.height)
            {
                return;
            }
            openEncoder(list)->setScissorRect(scissor);
            core::noteNativeCall(list.core);
            held.scissorKnown = true;
            held.scissor = scissor;
        }

        //==============================================================================================================
        /// Sets `reference` for both faces unless the encoder holds it.
        void applyStencilReference(CommandListImpl& list, uint32_t reference)
        {
            metal4::RenderHeld& held = list.draw.held;
            if (held.stencilReference == reference)
            {
                return;
            }
            openEncoder(list)->setStencilReferenceValue(reference);
            core::noteNativeCall(list.core);
            held.stencilReference = reference;
        }

        //==============================================================================================================
        /// Sets the blend color `color` unless the encoder holds it.
        void applyBlendColor(CommandListImpl& list, const std::array<float, 4>& color)
        {
            metal4::RenderHeld& held = list.draw.held;
            if (held.blendColor == color)
            {
                return;
            }
            openEncoder(list)->setBlendColor(color[0], color[1], color[2], color[3]);
            core::noteNativeCall(list.core);
            held.blendColor = color;
        }

        //==============================================================================================================
        /// Records the depth-bias values `bias`, as the encoder sees them now, and applies them when the pipeline it holds
        /// has `RasterState::depthBias`.
        void applyBiasValues(CommandListImpl& list, const metal4::DepthBias& bias)
        {
            list.draw.bias = bias;
            const metal4::PipelineState* pipeline = list.draw.held.pipeline;
            if (pipeline != nullptr && pipeline->depthBias)
            {
                metal4::applyDepthBias(list, bias);
            }
        }

        //==============================================================================================================
        /// `metal4::applyPipeline`, inlined into `setPipeline` so a bind on the open render encoder makes no call to reach it.
        [[gnu::always_inline]] inline void applyPipelineNow(CommandListImpl& list, const metal4::PipelineState& pipeline)
        {
            // The encoder is read once: the setters below leave it open.
            MTL4::RenderCommandEncoder* encoder = openEncoder(list);
            setRenderState(list, encoder, &pipeline);
            setDepthStencil(list, encoder, pipeline.depthStencil, pipeline.boundDepthStencil);
            setCull(list, encoder, pipeline.cull);
            if (pipeline.setsWinding)
            {
                setWinding(list, encoder, pipeline.winding);
            }
            setFill(list, encoder, pipeline.fill);
            setDepthClip(list, encoder, pipeline.depthClip);
            if (pipeline.depthBias)
            {
                metal4::applyDepthBias(list, list.draw.bias);
            }
            else if (list.draw.held.bias != metal4::DepthBias{})
            {
                // A pipeline without depth bias draws with none.
                metal4::applyDepthBias(list, metal4::DepthBias{});
            }
        }

        //==============================================================================================================
        /// Returns the payload of `pipeline`: from the list's resolved pair when it holds the handle, else read with the pool's
        /// acquire load, one entry replacing the older one.
        const metal4::PipelineState* resolvePipeline(CommandListImpl& list, Pipeline pipeline)
        {
            metal4::ResolvedPipelines& resolved = list.draw.resolved;
            if (pipeline.bits == resolved.handles[0])
            {
                return resolved.states[0];
            }
            if (pipeline.bits == resolved.handles[1])
            {
                return resolved.states[1];
            }
            // A record's payload is fixed from creation to retirement, so it is read with no lock (ADR 0020).
            const auto* state = static_cast<const metal4::PipelineState*>(list.core.device->pool<Pipeline>().native(pipeline));
            resolved.handles[1] = resolved.handles[0];
            resolved.states[1] = resolved.states[0];
            resolved.handles[0] = pipeline.bits;
            resolved.states[0] = state;
            return state;
        }

        //==============================================================================================================
        /// True when the render encoder of the open pass is open, so a state command applies now; otherwise it joins the
        /// prefix.
        bool appliesNow(const CommandListImpl& list)
        {
            return list.encoders.renderEncoder() != nullptr;
        }
    } // namespace

    namespace metal4 {
        //==============================================================================================================
        void applyPipeline(CommandListImpl& list, const PipelineState& pipeline)
        {
            applyPipelineNow(list, pipeline);
        }

        //==============================================================================================================
        void applyDepthBias(CommandListImpl& list, const DepthBias& bias)
        {
            RenderHeld& held = list.draw.held;
            if (held.bias == bias)
            {
                return;
            }
            openEncoder(list)->setDepthBias(bias.constant, bias.slopeScaled, bias.clamp);
            core::noteNativeCall(list.core);
            held.bias = bias;
        }

        //==============================================================================================================
        void applyRoot(CommandListImpl& list, uint64_t address)
        {
            if (list.draw.tableRoot == address)
            {
                return;
            }
            // The encoders take the table's bindings as they stand at each draw or dispatch, so the write needs no encoder.
            list.state.table->setAddress(address, 0);
            core::noteNativeCall(list.core);
            list.draw.tableRoot = address;
        }

        //==============================================================================================================
        void restoreHeld(CommandListImpl& list, const RenderHeld& previous)
        {
            MTL4::RenderCommandEncoder* encoder = openEncoder(list);
            if (previous.pipeline != nullptr)
            {
                setRenderState(list, encoder, previous.pipeline);
            }
            // The new encoder holds Metal's default, so only a state that tests something is sent, and it is its own bound one.
            setDepthStencil(list, encoder, previous.depthStencil, previous.depthStencil);
            setCull(list, encoder, previous.cull);
            setWinding(list, encoder, previous.winding);
            setFill(list, encoder, previous.fill);
            setDepthClip(list, encoder, previous.depthClip);
            applyDepthBias(list, previous.bias);
            if (previous.viewportKnown)
            {
                applyViewport(list, previous.viewport);
            }
            if (previous.scissorKnown)
            {
                applyScissor(list, previous.scissor);
            }
            applyStencilReference(list, previous.stencilReference);
            applyBlendColor(list, previous.blendColor);
        }

        //==============================================================================================================
        void replayPrefix(CommandListImpl& list)
        {
            for (const PrefixCommand& command : list.draw.prefix)
            {
                switch (command.kind)
                {
                case PrefixKind::Pipeline: applyPipeline(list, *command.pipeline); break;
                case PrefixKind::Viewport: applyViewport(list, command.viewport); break;
                case PrefixKind::Scissor: applyScissor(list, command.scissor); break;
                case PrefixKind::StencilReference: applyStencilReference(list, command.stencilReference); break;
                case PrefixKind::BlendColor: applyBlendColor(list, command.color); break;
                case PrefixKind::DepthBias: applyBiasValues(list, DepthBias{ command.bias[0], command.bias[1], command.bias[2] }); break;
                case PrefixKind::Root: applyRoot(list, command.root); break;
                case PrefixKind::Execute:
                case PrefixKind::DrawIndirect:
                case PrefixKind::IndexedIndirect:
                case PrefixKind::MeshExecute:
                case PrefixKind::MeshIndirect: replayCounted(list, command); break;
                }
            }
            list.draw.clearPrefix();
        }

        //==============================================================================================================
        MTL4::ComputeCommandEncoder* applyComputePipeline(CommandListImpl& list)
        {
            ROJORHI_INTERNAL_ASSERT(list.draw.compute != nullptr, "a dispatch is recorded with no compute pipeline bound");
            Encoders& encoders = list.encoders;
            MTL4::ComputeCommandEncoder* encoder = encoders.compute(list.core, list.state, MTL::StageDispatch);
            const MTL::ComputePipelineState* pipeline = list.draw.compute->compute;
            if (encoders.kernelPipeline() != pipeline)
            {
                encoder->setComputePipelineState(pipeline);
                core::noteNativeCall(list.core);
                encoders.setKernelPipeline(pipeline);
            }
            return encoder;
        }
    } // namespace metal4

    //==================================================================================================================
    void CommandList::setPipeline(Pipeline pipeline)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::bindPipeline(list.core, pipeline);
        const metal4::PipelineState* state = resolvePipeline(list, pipeline);
        metal4::DrawState& draw = list.draw;
        if (state->kind == core::PipelineKind::Compute)
        {
            // Binding opens the lazy compute encoder. Stage 0 emits no held-back barrier: no command follows yet.
            metal4::Encoders& encoders = list.encoders;
            MTL4::ComputeCommandEncoder* encoder = encoders.compute(list.core, list.state, 0);
            if (encoders.kernelPipeline() != state->compute)
            {
                encoder->setComputePipelineState(state->compute);
                core::noteNativeCall(list.core);
                encoders.setKernelPipeline(state->compute);
            }
            draw.compute = state;
            return;
        }
        draw.pipeline = state;
        draw.topology = state->listType;
        if (appliesNow(list))
        {
            applyPipelineNow(list, *state);
            return;
        }
        metal4::PrefixCommand command{ .kind = metal4::PrefixKind::Pipeline };
        command.pipeline = state;
        draw.record(command);
    }

    //==================================================================================================================
    void CommandList::setViewport(const Viewport& viewport)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setViewport(list.core, viewport);
        const MTL::Viewport native{ viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth };
        if (appliesNow(list))
        {
            applyViewport(list, native);
            return;
        }
        metal4::PrefixCommand command{ .kind = metal4::PrefixKind::Viewport };
        command.viewport = native;
        list.draw.record(command);
    }

    //==================================================================================================================
    void CommandList::setScissor(const Rect2D& rect)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setScissor(list.core, rect);
        const MTL::ScissorRect native{ rect.x, rect.y, rect.width, rect.height };
        if (appliesNow(list))
        {
            applyScissor(list, native);
            return;
        }
        metal4::PrefixCommand command{ .kind = metal4::PrefixKind::Scissor };
        command.scissor = native;
        list.draw.record(command);
    }

    //==================================================================================================================
    void CommandList::setStencilReference(uint8_t reference)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setStencilReference(list.core);
        if (appliesNow(list))
        {
            applyStencilReference(list, reference);
            return;
        }
        metal4::PrefixCommand command{ .kind = metal4::PrefixKind::StencilReference };
        command.stencilReference = reference;
        list.draw.record(command);
    }

    //==================================================================================================================
    void CommandList::setBlendConstants(float r, float g, float b, float a)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setBlendConstants(list.core);
        const std::array<float, 4> color{ r, g, b, a };
        if (appliesNow(list))
        {
            applyBlendColor(list, color);
            return;
        }
        metal4::PrefixCommand command{ .kind = metal4::PrefixKind::BlendColor };
        command.color = color;
        list.draw.record(command);
    }

    //==================================================================================================================
    void CommandList::setPrimitiveTopology(PrimitiveTopology topology)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setPrimitiveTopology(list.core, topology);
        // Metal takes the primitive type with each draw, so the topology is list state alone.
        list.draw.topology = metal4::primitiveType(topology);
    }

    //==================================================================================================================
    void CommandList::setDepthBias(float constant, float slopeScaled, float clamp)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setDepthBias(list.core);
        if (appliesNow(list))
        {
            applyBiasValues(list, metal4::DepthBias{ constant, slopeScaled, clamp });
            return;
        }
        metal4::PrefixCommand command{ .kind = metal4::PrefixKind::DepthBias };
        command.bias = { constant, slopeScaled, clamp };
        list.draw.record(command);
    }
} // namespace rojoRHI
