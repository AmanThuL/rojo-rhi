//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Scopes.cpp
/// @brief Implements the D3D12 scope entries: a PIX event with the UTF-16 label and a timestamp query at each end.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/RenderPasses.h"
#include "Core/ScopeRecord.h"
#include "D3D12Common.h"
#include "D3D12ListState.h"
#include "D3D12Timestamps.h"

#include <cstdint>
#include <string_view>

namespace rojoRHI {
    namespace {
        /// The legacy Unicode form of a PIX event's metadata: the data is a NUL-terminated UTF-16 string.
        constexpr UINT kUnicodeEvent = 0;
    } // namespace

    //==================================================================================================================
    void CommandList::beginScope(std::string_view label)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        // The core records the scope with its label, or its default label, which the record keeps.
        core::beginScope(list.core, label);
        d3d12::ListState& state = list.state;
        core::ScopeRecord& record = list.core.scopeLog.back();
        // The scratch is the list's own and grows to the longest label it has held, so a label converts without allocating.
        d3d12::widen(record.label, state.scratch);
        state.list->BeginEvent(kUnicodeEvent, state.scratch.c_str(), static_cast<UINT>((state.scratch.size() + 1) * sizeof(wchar_t)));
        core::noteNativeCall(list.core);
        d3d12::ListTimestamps& timestamps = state.timestamps;
        const uint32_t pair = timestamps.claimPair(list);
        timestamps.open.push_back(pair);
        if (pair == core::kNoTimestamp)
        {
            return;
        }
        const d3d12::ListTimestamps::Heap& heap = timestamps.heaps[pair / d3d12::ListTimestamps::kHeapQueries];
        state.list->EndQuery(heap.queries.get(), D3D12_QUERY_TYPE_TIMESTAMP, pair % d3d12::ListTimestamps::kHeapQueries);
        core::noteNativeCall(list.core);
        // The frame wait finds the list by its ordinal and the ticks by the pair.
        record.timestamp = (state.ordinal << 16) | pair;
    }

    //==================================================================================================================
    void CommandList::endScope()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::endScope(list.core);
        d3d12::ListState& state = list.state;
        d3d12::ListTimestamps& timestamps = state.timestamps;
        const uint32_t pair = timestamps.open.back();
        timestamps.open.pop_back();
        if (pair != core::kNoTimestamp)
        {
            const d3d12::ListTimestamps::Heap& heap = timestamps.heaps[pair / d3d12::ListTimestamps::kHeapQueries];
            state.list->EndQuery(heap.queries.get(), D3D12_QUERY_TYPE_TIMESTAMP, pair % d3d12::ListTimestamps::kHeapQueries + 1);
            core::noteNativeCall(list.core);
        }
        state.list->EndEvent();
        core::noteNativeCall(list.core);
    }
} // namespace rojoRHI
