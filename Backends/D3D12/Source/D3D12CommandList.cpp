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
        CommandListCore& coreOf(CommandList list)
        {
            return CommandListImpl::of(list).core;
        }
    } // namespace core

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
        // Reset leaves the native list's dynamic state unknown, so the list's first pass writes every starting value. The
        // pass description and the barrier scratch are kept: they describe native commands, not the list's state.
        state.dynamic = {};
        // Acquisition sends no heaps and no root signature, so a list that only copies, uploads or fills sends neither.
        // The heaps go before the first root signature that indexes them, set by the first graphics `setPipeline` or
        // the first dispatch, and before a storage clear's shader-visible view; the fill kernel's root signature
        // indexes no heap.
        state.binding.graphicsSignatureOwed = type == QueueType::Graphics;
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

    void CommandList::end()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::endList(list.core);
        d3d12::endSubmitTiming(list);
        d3d12::resolveTimestamps(list);
        d3d12::requireSucceeded(d3d12::deviceOf(list), list.state.list->Close(), "ID3D12GraphicsCommandList::Close in CommandList::end");
        core::noteNativeCall(list.core);
    }

    Addr CommandList::uploadFrameData(std::span<const std::byte> bytes, uint32_t alignment)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        // Only a list with no page, or a full one, takes the arena's lock to claim a page (ADR 0041).
        const core::ArenaAllocation place = core::allocateFrameData(*list.device, list, bytes, alignment, d3d12::createArenaPage, list.device);
        d3d12::copySmall(place.memory, bytes.data(), bytes.size());
        d3d12::LastUpload& last = impl.state.upload;
        if (place.page.bits != last.page.bits) [[unlikely]]
        {
            // The page's payload is fixed while the page lives, so it is read with acquire loads and no lock, once per page.
            const auto* payload = static_cast<const d3d12::BufferPayload*>(list.device->pool<Buffer>().native(place.page));
            last.page = place.page;
            last.viewIndex = payload->viewIndex;
            last.base = payload->base;
        }
        // The core kept the offset below maxAddrOffset.
        return d3d12::encodeAddr(last.viewIndex, place.offset);
    }

    void CommandList::setRoot(Addr root)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setRoot(list.core, root);
        // The address is bound lazily: the next draw binds it on the graphics side and the next dispatch on the compute side.
        // A root in the page of the list's last upload, as `pushRoot` binds, needs no lookup in the heap directory: the
        // page's raw-view index names it, and its base plus the offset is what the directory would give.
        d3d12::Binding& binding = list.state.binding;
        const d3d12::LastUpload& last = list.state.upload;
        binding.root =
            d3d12::addrInBuffer(root, last.viewIndex) ? d3d12::gpuAddress(last.base, root) : d3d12::rootAddress(d3d12::deviceOf(list).directory, root);
        binding.graphicsDirty |= d3d12::kGraphicsRootDirty;
        binding.computeRootDirty = true;
    }

    void CommandList::declareAccessChecked(std::span<const ResourceAccess> accesses)
    {
        // A record for the validation tracker only: no native call.
        core::declareAccess(CommandListImpl::of(*this).core, accesses);
    }
} // namespace rojoRHI
