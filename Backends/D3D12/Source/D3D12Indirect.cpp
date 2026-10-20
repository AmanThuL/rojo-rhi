//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Indirect.cpp
/// @brief Implements the indirect draw entries for the D3D12 backend: one `ExecuteIndirect` each, after the graphics flush.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Draws.h"
#include "Core/HandlePool.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12ListState.h"

#include <cstdint>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Records up to `maxCount` records of `signature` from `args` on `list`, whose graphics side is flushed: exactly
        /// `maxCount` when `count` is null, else as many as the `uint32` at `count` says, at most `maxCount`, which is
        /// `ExecuteIndirect`'s own rule. The signature writes each record's drawId to `b1`, so the native list holds the last
        /// drawn record's there afterwards, and the next direct draw writes 0 again.
        void execute(CommandListImpl& list, ID3D12CommandSignature* signature, Addr args, uint32_t maxCount, Addr count)
        {
            // A buffer's payload is published before its handle is returned, so the directory finds it with one acquire load.
            // The null address names the reserved view-heap slot 0, where the directory finds no buffer.
            const d3d12::HeapDirectory& directory = d3d12::deviceOf(list).directory;
            const d3d12::BufferPayload* records = directory.find(args.lo);
            ROJORHI_INTERNAL_ASSERT(records != nullptr, "the records of an indirect draw name no live buffer");
            const d3d12::BufferPayload* counted = directory.find(count.lo);
            ID3D12Resource* const countResource = counted == nullptr ? nullptr : counted->resource;
            const uint64_t countOffset = counted == nullptr ? 0u : count.hi;
            list.state.list->ExecuteIndirect(signature, maxCount, records->resource, args.hi, countResource, countOffset);
            core::noteNativeCall(list.core);
            list.state.binding.drawIdDirty = true;
        }
    } // namespace

    //==================================================================================================================
    void CommandList::drawIndirect(Addr args, uint32_t maxCount, Addr count)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::drawIndirect(list.core, args, maxCount, count);
        // A maxCount of 0 breaks a rule only Debug checks; a Release build records nothing for it.
        if (maxCount == 0)
        {
            return;
        }
        d3d12::flushGraphics(list);
        execute(list, d3d12::deviceOf(list).roots.draw(), args, maxCount, count);
    }

    //==================================================================================================================
    void CommandList::drawIndexedIndirect(Buffer indices, IndexType type, Addr args, uint32_t maxCount, Addr count)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::drawIndexedIndirect(list.core, indices, args, maxCount, count);
        if (maxCount == 0)
        {
            return;
        }
        d3d12::flushGraphics(list);
        // A record's payload is fixed from creation to retirement, so it is read with one load and no lock (ADR 0020).
        const auto& buffer = *static_cast<const d3d12::BufferPayload*>(list.core.device->pool<Buffer>().native(indices));
        // The records choose their first index and base vertex, so they read the same whole-buffer view a direct draw sets.
        d3d12::applyIndexBuffer(list, buffer, type);
        execute(list, d3d12::deviceOf(list).roots.drawIndexed(), args, maxCount, count);
    }

    //==================================================================================================================
    void CommandList::drawMeshTasksIndirect(Addr args, uint32_t maxCount, Addr count)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::drawMeshTasksIndirect(list.core, args, maxCount, count);
        if (maxCount == 0)
        {
            return;
        }
        // The task and mesh entries read drawId from b1 as the vertex stage does (ADR 0018), so the signature writes it here too.
        d3d12::flushGraphics(list);
        execute(list, d3d12::deviceOf(list).roots.mesh(), args, maxCount, count);
    }
} // namespace rojoRHI
