//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Dispatches.cpp
/// @brief Implements the dispatch entries for the D3D12 backend and the compute flush that runs before each.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Core/Draws.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12ListState.h"
#include "D3D12PipelineState.h"

#include <cstdint>

namespace rojoRHI {
    namespace d3d12 {
        //==============================================================================================================
        void flushCompute(CommandListImpl& list)
        {
            ListState& state = list.state;
            Binding& binding = state.binding;
            ROJORHI_INTERNAL_ASSERT(binding.pipeline != nullptr, "a dispatch is recorded with no compute pipeline bound");
            if (binding.computeRoot != ComputeRoot::Global)
            {
                // A backend kernel or vendor work left its own root signature; switching back drops every root argument, so
                // the root is bound again below.
                state.list->SetComputeRootSignature(deviceOf(list).roots.global());
                core::noteNativeCall(list.core);
                binding.computeRoot = ComputeRoot::Global;
                binding.computeRootDirty = true;
            }
            ID3D12PipelineState* const pso = binding.pipeline->pso;
            if (binding.computePipeline != pso)
            {
                state.list->SetPipelineState(pso);
                core::noteNativeCall(list.core);
                binding.computePipeline = pso;
                binding.graphicsPipeline = nullptr;
            }
            if (binding.computeRootDirty)
            {
                // A list with no root yet binds none: its shaders read no root block.
                if (binding.root != 0)
                {
                    state.list->SetComputeRootShaderResourceView(0, binding.root);
                    core::noteNativeCall(list.core);
                }
                binding.computeRootDirty = false;
            }
        }
    } // namespace d3d12

    //==================================================================================================================
    void CommandList::dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::dispatch(list.core, x, y, z);
        if (x == 0 || y == 0 || z == 0)
        {
            return;
        }
        d3d12::flushCompute(list);
        list.state.list->Dispatch(x, y, z);
        core::noteNativeCall(list.core);
    }

    //==================================================================================================================
    void CommandList::dispatchIndirect(Addr args)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::dispatchIndirect(list.core, args);
        d3d12::flushCompute(list);
        // The buffer's payload is published before its handle is returned, so the directory finds it with one acquire load.
        DeviceImpl& impl = d3d12::deviceOf(list);
        const d3d12::BufferPayload* buffer = impl.directory.find(args.lo);
        ROJORHI_INTERNAL_ASSERT(buffer != nullptr, "the arguments of an indirect dispatch name no live buffer");
        list.state.list->ExecuteIndirect(impl.roots.dispatch(), 1, buffer->resource, args.hi, nullptr, 0);
        core::noteNativeCall(list.core);
    }
} // namespace rojoRHI
