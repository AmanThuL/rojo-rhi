//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12GraphicsState.cpp
/// @brief Implements pipeline binding, the dynamic-state setters and the graphics flush before each draw for the D3D12 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/RenderPasses.h"
#include "D3D12Common.h"
#include "D3D12ListState.h"
#include "D3D12PipelineState.h"

#include <array>
#include <cstdint>
#include <optional>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Returns the D3D12 topology of `topology`.
        D3D_PRIMITIVE_TOPOLOGY topologyOf(PrimitiveTopology topology)
        {
            switch (topology)
            {
            case PrimitiveTopology::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            case PrimitiveTopology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case PrimitiveTopology::LineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            case PrimitiveTopology::TriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            case PrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            }
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }

        //==============================================================================================================
        /// True when `a` and `b` are the same viewport.
        bool sameViewport(const D3D12_VIEWPORT& a, const D3D12_VIEWPORT& b)
        {
            return a.TopLeftX == b.TopLeftX && a.TopLeftY == b.TopLeftY && a.Width == b.Width && a.Height == b.Height && a.MinDepth == b.MinDepth &&
                   a.MaxDepth == b.MaxDepth;
        }

        //==============================================================================================================
        /// True when `a` and `b` are the same rectangle.
        bool sameRect(const D3D12_RECT& a, const D3D12_RECT& b)
        {
            return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
        }

        //==============================================================================================================
        /// Sets `pso`, a graphics or mesh pipeline, in the one pipeline slot both sides of `list` share.
        void setGraphicsPso(CommandListImpl& list, ID3D12PipelineState* pso)
        {
            list.state.list->SetPipelineState(pso);
            core::noteNativeCall(list.core);
            d3d12::Binding& binding = list.state.binding;
            binding.graphicsPipeline = pso;
            binding.computePipeline = nullptr;
        }

        //==============================================================================================================
        /// Sends the depth bias `list` recorded. D3D12 takes the clamp before the slope-scaled term.
        void sendDepthBias(CommandListImpl& list)
        {
            const std::array<float, 3>& bias = list.state.dynamic.depthBias;
            list.state.list->RSSetDepthBias(bias[0], bias[2], bias[1]);
            core::noteNativeCall(list.core);
        }

        //==============================================================================================================
        /// Sets `topology` unless the native list of `list` holds it.
        void applyTopology(CommandListImpl& list, D3D_PRIMITIVE_TOPOLOGY topology)
        {
            D3D_PRIMITIVE_TOPOLOGY& held = list.state.dynamic.topology;
            if (held == topology)
            {
                return;
            }
            list.state.list->IASetPrimitiveTopology(topology);
            core::noteNativeCall(list.core);
            held = topology;
        }
    } // namespace

    namespace d3d12 {
        //==============================================================================================================
        void flushGraphics(CommandListImpl& list)
        {
            Binding& binding = list.state.binding;
            ROJORHI_INTERNAL_ASSERT(
                binding.pipeline != nullptr && binding.pipeline->kind != PipelineKind::Compute, "a draw is recorded with no graphics pipeline bound"
            );
            const PipelineState& pipeline = *binding.pipeline;
            if (binding.graphicsPipeline != pipeline.pso)
            {
                // Another pipeline took the slot: whether D3D12 keeps a dynamic depth bias across it is unverified, so the
                // recorded bias goes with the pipeline, as at `setPipeline`. The topology is input-assembler state and stays.
                setGraphicsPso(list, pipeline.pso);
                if (pipeline.depthBias)
                {
                    sendDepthBias(list);
                }
            }
            if (binding.graphicsRootDirty)
            {
                // A list with no root yet binds none: its shaders read no root block.
                if (binding.root != 0)
                {
                    list.state.list->SetGraphicsRootShaderResourceView(0, binding.root);
                    core::noteNativeCall(list.core);
                }
                binding.graphicsRootDirty = false;
            }
        }
    } // namespace d3d12

    //==================================================================================================================
    void CommandList::setPipeline(Pipeline pipeline)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::bindPipeline(list.core, pipeline);
        // A record's payload is fixed from creation to retirement, so it is read with one load and no lock (ADR 0020).
        const auto* state = static_cast<const d3d12::PipelineState*>(list.core.device->pool<Pipeline>().native(pipeline));
        d3d12::Binding& binding = list.state.binding;
        binding.pipeline = state;
        // One pipeline slot serves both sides of a D3D12 list, so a bind is skipped only when its own side holds the pipeline.
        if (state->kind == d3d12::PipelineKind::Compute)
        {
            if (binding.computePipeline != state->pso)
            {
                list.state.list->SetPipelineState(state->pso);
                core::noteNativeCall(list.core);
                binding.computePipeline = state->pso;
                binding.graphicsPipeline = nullptr;
            }
            return;
        }
        if (binding.graphicsPipeline != state->pso)
        {
            setGraphicsPso(list, state->pso);
        }
        // Binding resets the topology to the list form of the pipeline's class (chapter 06); a mesh pipeline has none.
        if (state->kind == d3d12::PipelineKind::Graphics)
        {
            applyTopology(list, state->listTopology);
        }
        // Whether D3D12 keeps a dynamic depth bias across SetPipelineState is unverified, so every bind sends it.
        if (state->depthBias)
        {
            sendDepthBias(list);
        }
    }

    //==================================================================================================================
    void CommandList::setViewport(const Viewport& viewport)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setViewport(list.core, viewport);
        const D3D12_VIEWPORT native{ viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth };
        std::optional<D3D12_VIEWPORT>& held = list.state.dynamic.viewport;
        if (held.has_value() && sameViewport(*held, native))
        {
            return;
        }
        list.state.list->RSSetViewports(1, &native);
        core::noteNativeCall(list.core);
        held = native;
    }

    //==================================================================================================================
    void CommandList::setScissor(const Rect2D& rect)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setScissor(list.core, rect);
        // D3D12 takes the right and bottom edges, both exclusive.
        const D3D12_RECT native{
            static_cast<LONG>(rect.x), static_cast<LONG>(rect.y), static_cast<LONG>(rect.x + rect.width), static_cast<LONG>(rect.y + rect.height)
        };
        std::optional<D3D12_RECT>& held = list.state.dynamic.scissor;
        if (held.has_value() && sameRect(*held, native))
        {
            return;
        }
        list.state.list->RSSetScissorRects(1, &native);
        core::noteNativeCall(list.core);
        held = native;
    }

    //==================================================================================================================
    void CommandList::setStencilReference(uint8_t reference)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setStencilReference(list.core);
        std::optional<uint8_t>& held = list.state.dynamic.stencilReference;
        if (held == reference)
        {
            return;
        }
        list.state.list->OMSetStencilRef(reference);
        core::noteNativeCall(list.core);
        held = reference;
    }

    //==================================================================================================================
    void CommandList::setBlendConstants(float r, float g, float b, float a)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setBlendConstants(list.core);
        const std::array<float, 4> factor{ r, g, b, a };
        std::optional<std::array<float, 4>>& held = list.state.dynamic.blendFactor;
        if (held == factor)
        {
            return;
        }
        list.state.list->OMSetBlendFactor(factor.data());
        core::noteNativeCall(list.core);
        held = factor;
    }

    //==================================================================================================================
    void CommandList::setPrimitiveTopology(PrimitiveTopology topology)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setPrimitiveTopology(list.core, topology);
        applyTopology(list, topologyOf(topology));
    }

    //==================================================================================================================
    void CommandList::setDepthBias(float constant, float slopeScaled, float clamp)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setDepthBias(list.core);
        const std::array<float, 3> bias{ constant, slopeScaled, clamp };
        std::array<float, 3>& recorded = list.state.dynamic.depthBias;
        // While a pipeline with depthBias is bound the native list holds the recorded values, so equal values send nothing.
        if (recorded == bias)
        {
            return;
        }
        recorded = bias;
        const d3d12::PipelineState* bound = list.state.binding.pipeline;
        // VU-rojo-pipeline-0025 makes this always true in Debug; Release does not check it, so the guard stays.
        if (bound != nullptr && bound->depthBias)
        {
            sendDepthBias(list);
        }
    }
} // namespace rojoRHI
