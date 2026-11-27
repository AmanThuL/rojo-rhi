//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Scopes.cpp
/// @brief Implements the D3D12 scope entries: a PIX event with debug markers and a timestamp query at each end.
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
            // PIX's legacy Unicode event metadata: the data is a NUL-terminated UTF-16 string.
            constexpr UINT kUnicodeEvent = 0;
        } // namespace

        void beginNativeScope(CommandListImpl& list, std::string_view label)
        {
            ListState& state = list.state;
            core::ScopeRecord& record = list.core.scopeLog.back();
            // The record's copy of a caller's label is in the frame arena, upload memory that a CPU read crosses uncached, so the
            // event widens the caller's label; an empty one takes the core's default, a static string. The list keeps its last
            // event's label, so a scope of the same text, as each pass of a loop has, widens nothing; the strings grow to the
            // longest label the list has held, so a label converts without allocating once they have.
            // A device without debug markers sends no event (ADR 0034); the scope is still recorded and, with scope timings, timed.
            if (list.core.device->marksScopes())
            {
                const std::string_view text = label.empty() ? record.label : label;
                if (text != state.eventSource)
                {
                    widen(text, state.eventLabel);
                    state.eventSource.assign(text);
                }
                state.list->BeginEvent(kUnicodeEvent, state.eventLabel.c_str(), static_cast<UINT>((state.eventLabel.size() + 1) * sizeof(wchar_t)));
                core::noteNativeCall(list.core);
            }
            // A closed gate claims nothing and pushes nothing for the scope's end to pop, so a list that times no scope
            // pays a test of the gate and no more. The first claim of a recording decides an unknown gate.
            ListTimestamps& timestamps = state.timestamps;
            if (timestamps.gate == ListTimestamps::Gate::Closed)
            {
                return;
            }
            const uint32_t pair = timestamps.claimPair(list);
            if (timestamps.gate == ListTimestamps::Gate::Closed)
            {
                return;
            }
            timestamps.open.push_back(pair);
            if (pair == core::kNoTimestamp)
            {
                return;
            }
            state.list->EndQuery(timestamps.heapOf(pair).queries.get(), D3D12_QUERY_TYPE_TIMESTAMP, timestamps.indexOf(pair));
            core::noteNativeCall(list.core);
            // The frame wait finds the list by its ordinal and the ticks by the pair.
            record.timestamp = (state.ordinal << 16) | pair;
        }

        void endNativeScope(CommandListImpl& list)
        {
            ListState& state = list.state;
            ListTimestamps& timestamps = state.timestamps;
            // The gate cannot close while a scope is open, so a closed one here pushed nothing at the scope's begin.
            if (timestamps.gate != ListTimestamps::Gate::Closed)
            {
                const uint32_t pair = timestamps.open.back();
                timestamps.open.pop_back();
                if (pair != core::kNoTimestamp)
                {
                    state.list->EndQuery(timestamps.heapOf(pair).queries.get(), D3D12_QUERY_TYPE_TIMESTAMP, timestamps.indexOf(pair) + 1);
                    core::noteNativeCall(list.core);
                }
            }
            if (list.core.device->marksScopes())
            {
                state.list->EndEvent();
                core::noteNativeCall(list.core);
            }
        }
    } // namespace d3d12

    void CommandList::beginScope(std::string_view label)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        // The core records the scope with its label, or its default label, which the record keeps.
        core::beginScope(list.core, label);
        d3d12::beginNativeScope(list, label);
    }

    void CommandList::endScope()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::endScope(list.core);
        d3d12::endNativeScope(list);
    }
} // namespace rojoRHI
