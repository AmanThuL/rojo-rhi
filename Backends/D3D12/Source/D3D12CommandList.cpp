//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12CommandList.cpp
/// @brief Implements D3D12 command list acquisition into native lists, `end`, frame data, the lazy root and declared accesses.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Core/Accesses.h"
#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Draws.h"
#include "Core/FrameArena.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "D3D12Addr.h"
#include "D3D12Buffers.h"
#include "D3D12Common.h"
#include "D3D12ListState.h"
#include "D3D12SubmitTiming.h"
#include "D3D12Timestamps.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace rojoRHI {
    namespace core {
        //==============================================================================================================
        CommandListCore& coreOf(CommandList list)
        {
            return CommandListImpl::of(list).core;
        }
    } // namespace core

    //==================================================================================================================
    CommandList Device::acquireCommandList(QueueType type, std::string_view label)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkAcquire(impl.core);
        const uint32_t slot = static_cast<uint32_t>(impl.core.frameNumber() % impl.core.framesInFlight());
        CommandListImpl& list = impl.lists.acquire(impl, slot, type);
        core::beginList(list.core, impl.core, type, label);
        d3d12::ListState& state = list.state;
        // The allocator is the list's own and was reset when the slot was reused, so no other list records into it.
        d3d12::requireSucceeded(impl, state.list->Reset(state.allocator.get(), nullptr), "ID3D12GraphicsCommandList::Reset in Device::acquireCommandList");
        core::noteNativeCall(list.core);
        d3d12::beginSubmitTiming(list);
        state.timestamps.reset();
        state.binding = {};
        state.upload = {};
        state.indirect = {};
        state.pass.reusable = false;
        state.barriers.single = d3d12::SingleBarrier::None;
        // Reset leaves the native list's dynamic state unknown, so the list's first pass writes every starting value.
        state.dynamic = {};
        if (type != QueueType::Copy)
        {
            // Shaders index both heaps directly, so they are set once per recording, before any root signature.
            ID3D12DescriptorHeap* const heaps[] = { impl.descriptors.views(), impl.descriptors.samplers() };
            state.list->SetDescriptorHeaps(2, heaps);
            core::noteNativeCall(list.core);
            if (type == QueueType::Graphics)
            {
                state.list->SetGraphicsRootSignature(impl.roots.global());
                core::noteNativeCall(list.core);
            }
            // The compute side takes the global root signature at its first dispatch, so a list that dispatches nothing
            // sets none.
        }
        // The native list keeps its name across recordings, so a list acquired again with the same label sends none.
        if (state.label != list.core.label)
        {
            d3d12::widen(list.core.label, state.scratch);
            d3d12::requireSucceeded(impl, state.list->SetName(state.scratch.c_str()), "ID3D12Object::SetName in Device::acquireCommandList");
            core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
            state.label = list.core.label;
        }
        return CommandListImpl::wrap(&list);
    }

    //==================================================================================================================
    void CommandList::end()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        // The core's checks run before any native call.
        core::endList(list.core);
        d3d12::endSubmitTiming(list);
        d3d12::resolveTimestamps(list);
        d3d12::requireSucceeded(d3d12::deviceOf(list), list.state.list->Close(), "ID3D12GraphicsCommandList::Close in CommandList::end");
        core::noteNativeCall(list.core);
    }

    //==================================================================================================================
    Addr CommandList::uploadFrameData(std::span<const std::byte> bytes, uint32_t alignment)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        // Only a list with no page, or a full one, takes the arena's lock to claim a page (ADR 0020).
        const core::ArenaAllocation place = core::allocateFrameData(*list.device, list, bytes, alignment, d3d12::createArenaPage, list.device);
        d3d12::copySmall(place.memory, bytes.data(), bytes.size());
        d3d12::LastUpload& last = impl.state.upload;
        if (place.page.bits != last.page.bits)
        {
            // The page's payload is fixed while the page lives, so it is read with one acquire load and no lock.
            last.page = place.page;
            last.payload = static_cast<const d3d12::BufferPayload*>(list.device->pool<Buffer>().native(place.page));
        }
        last.address = d3d12::encodeAddr(*last.payload, place.offset);
        last.gpu = last.payload->base + place.offset;
        return last.address;
    }

    //==================================================================================================================
    void CommandList::setRoot(Addr root)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setRoot(list.core, root);
        // The address is bound lazily: the next draw binds it on the graphics side and the next dispatch on the compute side.
        // A root the list uploaded last, as `pushRoot` binds, needs no lookup in the heap directory.
        d3d12::Binding& binding = list.state.binding;
        const d3d12::LastUpload& last = list.state.upload;
        binding.root =
            root.lo == last.address.lo && root.hi == last.address.hi && last.gpu != 0 ? last.gpu : d3d12::rootAddress(d3d12::deviceOf(list).directory, root);
        binding.graphicsRootDirty = true;
        binding.computeRootDirty = true;
    }

    //==================================================================================================================
    void CommandList::declareAccessChecked(std::span<const ResourceAccess> accesses)
    {
        // A record for the validation tracker only: no native call.
        core::declareAccess(CommandListImpl::of(*this).core, accesses);
    }
} // namespace rojoRHI
