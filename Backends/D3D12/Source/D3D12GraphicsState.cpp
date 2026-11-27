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

        bool sameViewport(const D3D12_VIEWPORT& a, const D3D12_VIEWPORT& b)
        {
            return a.TopLeftX == b.TopLeftX && a.TopLeftY == b.TopLeftY && a.Width == b.Width && a.Height == b.Height && a.MinDepth == b.MinDepth &&
                   a.MaxDepth == b.MaxDepth;
        }

        bool sameRect(const D3D12_RECT& a, const D3D12_RECT& b)
        {
            return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
        }

        // Both sides of a D3D12 list share one pipeline slot.
        void setGraphicsPso(CommandListImpl& list, ID3D12PipelineState* pso)
        {
            list.state.list->SetPipelineState(pso);
            core::noteNativeCall(list.core);
            d3d12::Binding& binding = list.state.binding;
            binding.graphicsPipeline = pso;
            binding.computePipeline = nullptr;
        }

        // D3D12 takes the clamp before the slope-scaled term.
        void sendDepthBias(CommandListImpl& list)
        {
            const std::array<float, 3>& bias = list.state.dynamic.depthBias;
            list.state.list->RSSetDepthBias(bias[0], bias[2], bias[1]);
            core::noteNativeCall(list.core);
        }

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

        // A hit selects its entry without a branch on which entry holds the handle: a recording that alternates two
        // pipelines flips that branch at every bind, and the predictor misses it once the driver's work between two binds
        // grows. A miss reads the payload through the handle pool's table, with acquire loads and no lock (ADR 0041).
        const d3d12::PipelineState* resolvePipeline(CommandListImpl& list, Pipeline pipeline)
        {
            d3d12::ResolvedPipelines& resolved = list.state.resolved;
            const bool older = pipeline.bits == resolved.handles[1];
            if ((pipeline.bits == resolved.handles[0]) | older) [[likely]]
            {
                return resolved.states[older ? 1 : 0];
            }
            const auto* state = static_cast<const d3d12::PipelineState*>(list.core.device->pool<Pipeline>().native(pipeline));
            resolved.handles[1] = resolved.handles[0];
            resolved.states[1] = resolved.states[0];
            resolved.handles[0] = pipeline.bits;
            resolved.states[0] = state;
            return state;
        }
    } // namespace

    namespace d3d12 {
        void flushGraphics(CommandListImpl& list, uint32_t reasons)
        {
            Binding& binding = list.state.binding;
            ROJORHI_INTERNAL_ASSERT(
                binding.pipeline != nullptr && binding.pipeline->kind != PipelineKind::Compute, "a draw is recorded with no graphics pipeline bound"
            );
            // A render pass begins with no pipeline bound and a draw needs one (chapter 07, VU-rojo-cmd-0023), so every draw
            // follows a graphics setPipeline in its own pass, which set any root signature acquisition or vendor work left
            // owed. Vendor work cannot be recorded inside a pass, so none comes between that setPipeline and this draw.
            ROJORHI_INTERNAL_ASSERT(!binding.graphicsSignatureOwed, "a draw follows no graphics setPipeline since acquisition or vendor work");
            uint32_t dirty = binding.graphicsDirty;
            if ((dirty & reasons) == 0) [[likely]]
            {
                return;
            }
            const PipelineState& pipeline = *binding.pipeline;
            // The bit says the slot may hold another pipeline; whether it does is still the slot's own test.
            if ((dirty & kGraphicsPipelineDirty) != 0 && binding.graphicsPipeline != pipeline.pso)
            {
                // Another pipeline took the slot: whether D3D12 keeps a dynamic depth bias across it is unverified, so the
                // recorded bias goes with the pipeline, as at `setPipeline`. The topology is input-assembler state and stays.
                setGraphicsPso(list, pipeline.pso);
                if (pipeline.depthBias)
                {
                    sendDepthBias(list);
                }
            }
            if ((dirty & kGraphicsRootDirty) != 0)
            {
                // A list with no root yet binds none: its shaders read no root block.
                if (binding.root != 0)
                {
                    list.state.list->SetGraphicsRootShaderResourceView(0, binding.root);
                    core::noteNativeCall(list.core);
                }
            }
            DynamicState& dynamic = list.state.dynamic;
            if ((dirty & kStencilReferenceDirty) != 0 && dynamic.stencilReferenceOwed && pipeline.readsStencilReference) [[unlikely]]
            {
                list.state.list->OMSetStencilRef(0);
                core::noteNativeCall(list.core);
                dynamic.stencilReference = uint8_t{ 0 };
                dynamic.stencilReferenceOwed = false;
            }
            if ((dirty & kBlendFactorDirty) != 0 && dynamic.blendFactorOwed && pipeline.readsBlendFactor) [[unlikely]]
            {
                const std::array<float, 4> zeros = {};
                list.state.list->OMSetBlendFactor(zeros.data());
                core::noteNativeCall(list.core);
                dynamic.blendFactor = zeros;
                dynamic.blendFactorOwed = false;
            }
            if ((dirty & reasons & kDrawIdDirty) != 0)
            {
                list.state.list->SetGraphicsRoot32BitConstant(1, 0, 0);
                core::noteNativeCall(list.core);
            }
            // Every reason but b1 is settled whichever draw this is; an indirect draw leaves b1 to the next direct one.
            binding.graphicsDirty = dirty & ~reasons;
        }
    } // namespace d3d12

    void CommandList::setPipeline(Pipeline pipeline)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::bindPipeline(list.core, pipeline);
        // A record's payload is fixed from creation to retirement, so it is read with no lock (ADR 0041), and the list's
        // last two binds need no table at all.
        const d3d12::PipelineState* state = resolvePipeline(list, pipeline);
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
                binding.graphicsDirty |= d3d12::kGraphicsPipelineDirty;
            }
            return;
        }
        if (binding.graphicsSignatureOwed) [[unlikely]]
        {
            // The list's first graphics pipeline, or its first after vendor work: no draw comes before this bind in its
            // pass, so the global root signature is set here and the draw tests nothing for it. It indexes the heaps
            // directly, so they go first. Setting it drops every root argument, so the root and the draw-record
            // constant are bound again at the next draw.
            if (!binding.heapsSet)
            {
                d3d12::setHeaps(list);
            }
            list.state.list->SetGraphicsRootSignature(d3d12::deviceOf(list).roots.global());
            core::noteNativeCall(list.core);
            binding.graphicsSignatureOwed = false;
            binding.graphicsDirty |= d3d12::kGraphicsRootDirty | d3d12::kDrawIdDirty;
        }
        if (binding.graphicsPipeline != state->pso)
        {
            setGraphicsPso(list, state->pso);
        }
        // The slot holds this pipeline now. A pass begins with no pipeline bound, so here is where a pass's owed
        // starting values become owed to draws, and only to those of a pipeline that reads them.
        const d3d12::DynamicState& dynamic = list.state.dynamic;
        const uint32_t owed = (dynamic.stencilReferenceOwed && state->readsStencilReference ? d3d12::kStencilReferenceDirty : 0u) |
                              (dynamic.blendFactorOwed && state->readsBlendFactor ? d3d12::kBlendFactorDirty : 0u);
        binding.graphicsDirty = (binding.graphicsDirty & ~(d3d12::kGraphicsPipelineDirty | d3d12::kStencilReferenceDirty | d3d12::kBlendFactorDirty)) | owed;
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

    void CommandList::setStencilReference(uint8_t reference)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setStencilReference(list.core);
        d3d12::DynamicState& dynamic = list.state.dynamic;
        if (dynamic.stencilReferenceOwed && reference == 0)
        {
            // The pass's starting value, still owed: the first draw that reads it sends it.
            return;
        }
        // Any other value replaces the starting one.
        dynamic.stencilReferenceOwed = false;
        list.state.binding.graphicsDirty &= ~d3d12::kStencilReferenceDirty;
        std::optional<uint8_t>& held = dynamic.stencilReference;
        if (held == reference)
        {
            return;
        }
        list.state.list->OMSetStencilRef(reference);
        core::noteNativeCall(list.core);
        held = reference;
    }

    void CommandList::setBlendConstants(float r, float g, float b, float a)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setBlendConstants(list.core);
        const std::array<float, 4> factor{ r, g, b, a };
        d3d12::DynamicState& dynamic = list.state.dynamic;
        if (dynamic.blendFactorOwed && factor == std::array<float, 4>{})
        {
            // The pass's starting value, still owed: the first draw that reads it sends it.
            return;
        }
        dynamic.blendFactorOwed = false;
        list.state.binding.graphicsDirty &= ~d3d12::kBlendFactorDirty;
        std::optional<std::array<float, 4>>& held = dynamic.blendFactor;
        if (held == factor)
        {
            return;
        }
        list.state.list->OMSetBlendFactor(factor.data());
        core::noteNativeCall(list.core);
        held = factor;
    }

    void CommandList::setPrimitiveTopology(PrimitiveTopology topology)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setPrimitiveTopology(list.core, topology);
        applyTopology(list, topologyOf(topology));
    }

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
