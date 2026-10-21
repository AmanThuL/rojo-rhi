//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Draws.cpp
/// @brief Implements the direct draw entries for the D3D12 backend and the index buffer every indexed draw binds.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Draws.h"
#include "Core/HandlePool.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12ListState.h"

#include <algorithm>
#include <cstdint>

namespace rojoRHI {
    namespace d3d12 {
        namespace {
            //==========================================================================================================
            /// Returns the view of the whole of `buffer` as indices of `type`, from offset 0: the indices of `type` that fit in
            /// the buffer and in the view's 32-bit size.
            D3D12_INDEX_BUFFER_VIEW indexView(const BufferPayload& buffer, IndexType type)
            {
                const uint32_t indexSize = type == IndexType::UInt16 ? 2 : 4;
                const auto size = static_cast<uint32_t>(std::min<uint64_t>(buffer.size, UINT32_MAX));
                D3D12_INDEX_BUFFER_VIEW view = {};
                view.BufferLocation = buffer.base;
                view.SizeInBytes = size - size % indexSize;
                view.Format = type == IndexType::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
                return view;
            }
        } // namespace

        //==============================================================================================================
        void applyIndexBuffer(CommandListImpl& list, const BufferPayload& buffer, IndexType type)
        {
            const D3D12_INDEX_BUFFER_VIEW view = indexView(buffer, type);
            D3D12_INDEX_BUFFER_VIEW& held = list.state.dynamic.indexBuffer;
            if (held.BufferLocation == view.BufferLocation && held.SizeInBytes == view.SizeInBytes && held.Format == view.Format)
            {
                return;
            }
            list.state.list->IASetIndexBuffer(&view);
            core::noteNativeCall(list.core);
            held = view;
        }
    } // namespace d3d12

    namespace {
        //==============================================================================================================
        /// Brings the graphics side of `list` to the caller's binding and writes 0 to the draw-record constant `b1`, which
        /// `rojoRHI::drawId` reads for a direct draw, unless the native list holds 0 there: after acquisition it holds no
        /// value, and after an indirect draw the last record's.
        void prepareDirect(CommandListImpl& list)
        {
            d3d12::flushGraphics(list);
            d3d12::Binding& binding = list.state.binding;
            if (binding.drawIdDirty)
            {
                list.state.list->SetGraphicsRoot32BitConstant(1, 0, 0);
                core::noteNativeCall(list.core);
                binding.drawIdDirty = false;
            }
        }
    } // namespace

    //==================================================================================================================
    void CommandList::draw(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::draw(list.core, vertices, instances);
        if (vertices == 0 || instances == 0)
        {
            return;
        }
        prepareDirect(list);
        // SV_InstanceID excludes the start instance, so firstInstance is invisible to shaders, as chapter 05 requires.
        list.state.list->DrawInstanced(vertices, instances, firstVertex, firstInstance);
        core::noteNativeCall(list.core);
    }

    //==================================================================================================================
    void CommandList::drawIndexed(
        Buffer indices, IndexType type, uint32_t count, uint32_t instances, uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance
    )
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::drawIndexed(list.core, indices, type, count, instances, firstIndex);
        if (count == 0 || instances == 0)
        {
            return;
        }
        prepareDirect(list);
        // A record's payload is fixed from creation to retirement, so it is read with one load and no lock (ADR 0020).
        const auto& buffer = *static_cast<const d3d12::BufferPayload*>(list.core.device->pool<Buffer>().native(indices));
        // The view covers the whole buffer, so draws of one buffer share it and firstIndex is the draw's start index.
        d3d12::applyIndexBuffer(list, buffer, type);
        list.state.list->DrawIndexedInstanced(count, instances, firstIndex, baseVertex, firstInstance);
        core::noteNativeCall(list.core);
    }

    //==================================================================================================================
    void CommandList::drawMeshTasks(uint32_t x, uint32_t y, uint32_t z)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::drawMeshTasks(list.core, x, y, z);
        if (x == 0 || y == 0 || z == 0)
        {
            return;
        }
        // The task and mesh entries read drawId from b1 as the vertex stage does (ADR 0018).
        prepareDirect(list);
        list.state.list->DispatchMesh(x, y, z);
        core::noteNativeCall(list.core);
    }
} // namespace rojoRHI
