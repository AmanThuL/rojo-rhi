//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Scopes.cpp
/// @brief Implements the D3D12 scope entries: a PIX event with the UTF-16 label and a timestamp query at each end.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Scopes.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/RenderPasses.h"
#include "Core/ScopeRecord.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12ListState.h"
#include "D3D12Timestamps.h"

#include <cstdint>
#include <string_view>

namespace rojoRHI {
    namespace d3d12 {
        namespace {
            /// The legacy Unicode form of a PIX event's metadata: the data is a NUL-terminated UTF-16 string.
            constexpr UINT kUnicodeEvent = 0;
        } // namespace

        //==============================================================================================================
        void beginNativeScope(CommandListImpl& list, std::string_view label)
        {
            ListState& state = list.state;
            core::ScopeRecord& record = list.core.scopeLog.back();
            // The record's copy of a caller's label is in the frame arena, upload memory that a CPU read crosses uncached, so the
            // event widens the caller's label; an empty one takes the core's default, a static string. The list keeps its last
            // event's label, so a scope of the same text, as each pass of a loop has, widens nothing; the strings grow to the
            // longest label the list has held, so a label converts without allocating once they have.
            const std::string_view text = label.empty() ? record.label : label;
            if (text != state.eventSource)
            {
                widen(text, state.eventLabel);
                state.eventSource.assign(text);
            }
            state.list->BeginEvent(kUnicodeEvent, state.eventLabel.c_str(), static_cast<UINT>((state.eventLabel.size() + 1) * sizeof(wchar_t)));
            core::noteNativeCall(list.core);
            ListTimestamps& timestamps = state.timestamps;
            const uint32_t pair = timestamps.claimPair(list);
            timestamps.open.push_back(pair);
            if (pair == core::kNoTimestamp)
            {
                return;
            }
            const ListTimestamps::Heap& heap = timestamps.heaps[pair / ListTimestamps::kHeapQueries];
            state.list->EndQuery(heap.queries.get(), D3D12_QUERY_TYPE_TIMESTAMP, pair % ListTimestamps::kHeapQueries);
            core::noteNativeCall(list.core);
            // The frame wait finds the list by its ordinal and the ticks by the pair.
            record.timestamp = (state.ordinal << 16) | pair;
        }

        //==============================================================================================================
        void endNativeScope(CommandListImpl& list)
        {
            ListState& state = list.state;
            ListTimestamps& timestamps = state.timestamps;
            const uint32_t pair = timestamps.open.back();
            timestamps.open.pop_back();
            if (pair != core::kNoTimestamp)
            {
                const ListTimestamps::Heap& heap = timestamps.heaps[pair / ListTimestamps::kHeapQueries];
                state.list->EndQuery(heap.queries.get(), D3D12_QUERY_TYPE_TIMESTAMP, pair % ListTimestamps::kHeapQueries + 1);
                core::noteNativeCall(list.core);
            }
            state.list->EndEvent();
            core::noteNativeCall(list.core);
        }
    } // namespace d3d12

    //==================================================================================================================
    void CommandList::beginScope(std::string_view label)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        // The core records the scope with its label, or its default label, which the record keeps.
        core::beginScope(list.core, label);
        d3d12::beginNativeScope(list, label);
    }

    //==================================================================================================================
    void CommandList::endScope()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::endScope(list.core);
        d3d12::endNativeScope(list);
    }
} // namespace rojoRHI
