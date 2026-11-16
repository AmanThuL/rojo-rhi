//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Fills.cpp
/// @brief Implements the D3D12 buffer fill, which dispatches the backend's fill kernel once per 2 GiB window of the range.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Copies.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12Kernels.h"
#include "D3D12ListState.h"

#include <algorithm>
#include <cstdint>

namespace rojoRHI {
    //==================================================================================================================
    void CommandList::fillBuffer(Buffer dst, uint64_t offset, uint64_t size, uint32_t pattern)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::fillBuffer(list.core, dst, offset, size);
        DeviceImpl& impl = d3d12::deviceOf(list);
        d3d12::ListState& state = list.state;
        d3d12::Binding& binding = state.binding;
        ID3D12PipelineState* fill = impl.kernels.fill();
        // The payload is fixed from creation to retirement, so it is read with one acquire load and no lock.
        const auto& payload = *static_cast<const d3d12::BufferPayload*>(list.core.device->pool<Buffer>().native(dst));
        // A list's compute side keeps the kernel bound until a dispatch rebinds the caller's, so fills in a row bind it once.
        if (binding.computeRoot != d3d12::ComputeRoot::Kernel || binding.computePipeline != fill)
        {
            state.list->SetComputeRootSignature(impl.roots.kernel());
            core::noteNativeCall(list.core);
            state.list->SetPipelineState(fill);
            core::noteNativeCall(list.core);
            binding.computeRoot = d3d12::ComputeRoot::Kernel;
            binding.computePipeline = fill;
            // A list holds one pipeline for both sides, so the fill's replaces any graphics pipeline.
            binding.graphicsPipeline = nullptr;
        }
        // The compute side holds the kernel root signature, so the next dispatch binds the global one and the root again.
        binding.computeRootDirty = true;
        // Each window of at most 2 GiB is a root view at its first byte, so a fill takes no descriptor.
        const uint64_t rangeEnd = offset + size;
        for (uint64_t start = offset; start < rangeEnd;)
        {
            const uint64_t windowEnd = std::min(rangeEnd, start + d3d12::kFillWindowBytes);
            const d3d12::FillConstants constants{ .pattern = pattern, .wordCount = static_cast<uint32_t>((windowEnd - start) / 4) };
            state.list->SetComputeRootUnorderedAccessView(1, payload.base + start);
            core::noteNativeCall(list.core);
            state.list->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
            core::noteNativeCall(list.core);
            state.list->Dispatch(d3d12::fillGroups(constants.wordCount), 1, 1);
            core::noteNativeCall(list.core);
            start = windowEnd;
        }
    }
} // namespace rojoRHI
