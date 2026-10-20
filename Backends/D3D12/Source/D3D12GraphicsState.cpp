//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12GraphicsState.cpp
/// @brief Implements pipeline binding for the D3D12 backend: a compute pipeline binds at once; a graphics one aborts.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/RenderPasses.h"
#include "D3D12Common.h"
#include "D3D12ListState.h"
#include "D3D12PipelineState.h"

namespace rojoRHI {
    //==================================================================================================================
    void CommandList::setPipeline(Pipeline pipeline)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::bindPipeline(list.core, pipeline);
        // A record's payload is fixed from creation to retirement, so it is read with one load and no lock (ADR 0020).
        const auto* state = static_cast<const d3d12::PipelineState*>(list.core.device->pool<Pipeline>().native(pipeline));
        if (state->kind != d3d12::PipelineKind::Compute)
        {
            d3d12::notImplemented("CommandList::setPipeline (graphics)");
        }
        // One pipeline slot serves both sides of a D3D12 list, so the bind is skipped only when the compute side holds it.
        d3d12::Binding& binding = list.state.binding;
        if (binding.computePipeline != state->pso)
        {
            list.state.list->SetPipelineState(state->pso);
            core::noteNativeCall(list.core);
            binding.computePipeline = state->pso;
            binding.graphicsPipeline = nullptr;
        }
        binding.pipeline = state;
    }
} // namespace rojoRHI
