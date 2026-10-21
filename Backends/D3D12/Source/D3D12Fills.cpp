//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Fills.cpp
/// @brief Implements the D3D12 buffer fill, which dispatches the backend's fill kernel once per 512 MiB window of the range.
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
#include "D3D12Descriptors.h"
#include "D3D12Kernels.h"
#include "D3D12ListState.h"

#include <algorithm>
#include <cstdint>

namespace rojoRHI {
    namespace {
        /// The bytes one raw view reaches, 2^29: the range of a fill is split into windows at multiples of it.
        constexpr uint64_t kFillWindowBytes = d3d12::kMaxAddrOffset + 1;

        //==============================================================================================================
        /// Returns a raw UAV over the `words` four-byte words of a buffer from word `firstWord`.
        D3D12_UNORDERED_ACCESS_VIEW_DESC windowUav(uint64_t firstWord, uint64_t words)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.FirstElement = firstWord;
            desc.Buffer.NumElements = static_cast<UINT>(words);
            desc.Buffer.StructureByteStride = 0;
            desc.Buffer.CounterOffsetInBytes = 0;
            desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            return desc;
        }
    } // namespace

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
        const uint64_t rangeEnd = offset + size;
        for (uint64_t start = offset; start < rangeEnd;)
        {
            const uint64_t windowEnd = std::min(rangeEnd, (start / kFillWindowBytes + 1) * kFillWindowBytes);
            d3d12::FillConstants constants;
            constants.pattern = pattern;
            constants.wordCount = static_cast<uint32_t>((windowEnd - start) / 4);
            if (start < kFillWindowBytes)
            {
                // The buffer's own raw UAV, next to its raw SRV, reaches the first window.
                constants.uavIndex = payload.viewIndex + 1;
                constants.firstWord = static_cast<uint32_t>(start / 4);
            }
            else
            {
                // A window past the raw view's reach gets a view of exactly that window from the frame slot's ring.
                const d3d12::TransientSlot slot = impl.descriptors.claimTransient(state.frameSlot);
                const D3D12_UNORDERED_ACCESS_VIEW_DESC uav = windowUav(start / 4, constants.wordCount);
                impl.device()->CreateUnorderedAccessView(payload.resource, nullptr, &uav, slot.cpu);
                core::DeviceCounts::add(impl.core.counts().descriptorWrites);
                constants.uavIndex = slot.index;
                constants.firstWord = 0;
            }
            state.list->SetComputeRoot32BitConstants(0, 4, &constants, 0);
            core::noteNativeCall(list.core);
            state.list->Dispatch(d3d12::fillGroups(constants.wordCount), 1, 1);
            core::noteNativeCall(list.core);
            start = windowEnd;
        }
    }
} // namespace rojoRHI
