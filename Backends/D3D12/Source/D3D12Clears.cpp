//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Clears.cpp
/// @brief Implements the D3D12 texture clear: render-target, depth-stencil or unordered-access clears between two transitions.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Copies.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Formats.h"
#include "Core/HandlePool.h"
#include "D3D12Barriers.h"
#include "D3D12Common.h"
#include "D3D12Descriptors.h"
#include "D3D12Formats.h"
#include "D3D12ListState.h"
#include "D3D12Textures.h"

#include <algorithm>
#include <cstdint>

namespace rojoRHI {
    namespace {
        /// How a clear reaches its subresources: the view kind it clears through, with the sync, access and layout the
        /// subresources take for it.
        struct ClearPath
        {
            D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;            ///< The sync scope of the clear.
            D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_NO_ACCESS; ///< The access the clear writes with.
            D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_UNDEFINED; ///< The layout the clear needs.
        };

        /// The path of a clear through the texture's render-target views.
        constexpr ClearPath kRenderTargetPath = { D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET };

        /// The path of a clear through the texture's depth-stencil views.
        constexpr ClearPath kDepthStencilPath = { D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                                                  D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                                                  D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE };

        /// The path of a clear through a transient unordered-access view.
        constexpr ClearPath kUnorderedAccessPath = { D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,
                                                     D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                                     D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS };

        //==============================================================================================================
        /// Returns the depth slices of mip `mip` of a 3D texture whose mip 0 has `depth` slices.
        uint32_t slicesOf(uint32_t depth, uint32_t mip)
        {
            return std::max(depth >> mip, 1u);
        }

        //==============================================================================================================
        /// Records one native barrier on `list` that moves `range` of `resource` from the sync, access and layout of `before`
        /// to those of `after`, counted as one native call and one native barrier.
        void transition(
            CommandListImpl& list, ID3D12Resource* resource, const D3D12_BARRIER_SUBRESOURCE_RANGE& range, const ClearPath& before, const ClearPath& after
        )
        {
            D3D12_TEXTURE_BARRIER barrier = {};
            barrier.SyncBefore = before.sync;
            barrier.SyncAfter = after.sync;
            barrier.AccessBefore = before.access;
            barrier.AccessAfter = after.access;
            barrier.LayoutBefore = before.layout;
            barrier.LayoutAfter = after.layout;
            barrier.pResource = resource;
            barrier.Subresources = range;
            barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
            D3D12_BARRIER_GROUP group = {};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = 1;
            group.pTextureBarriers = &barrier;
            list.state.list->Barrier(1, &group);
            core::noteNativeCall(list.core);
            ++list.core.counts.nativeBarriers;
        }

        //==============================================================================================================
        /// Returns the unordered-access view of mip `mip` of `payload`'s texture over the layers `range` covers, or over every
        /// depth slice of the mip for a 3D texture. A UAV has no cube dimension, so a cube type is the 2D array of its faces.
        /// A Storage texture has one sample.
        D3D12_UNORDERED_ACCESS_VIEW_DESC clearView(const d3d12::TexturePayload& payload, uint32_t mip, const D3D12_BARRIER_SUBRESOURCE_RANGE& range)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
            view.Format = d3d12::viewFormat(payload.format, Format::Undefined, TextureAspect::Color, TextureUsage::Storage);
            switch (payload.type)
            {
            case TextureType::Tex1D:
            case TextureType::Tex1DArray:
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                view.Texture1DArray.MipSlice = mip;
                view.Texture1DArray.FirstArraySlice = range.FirstArraySlice;
                view.Texture1DArray.ArraySize = range.NumArraySlices;
                break;
            case TextureType::Tex3D:
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                view.Texture3D.MipSlice = mip;
                view.Texture3D.FirstWSlice = 0;
                view.Texture3D.WSize = slicesOf(payload.depth, mip);
                break;
            case TextureType::Tex2D:
            case TextureType::Tex2DArray:
            case TextureType::Cube:
            case TextureType::CubeArray:
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.MipSlice = mip;
                view.Texture2DArray.FirstArraySlice = range.FirstArraySlice;
                view.Texture2DArray.ArraySize = range.NumArraySlices;
                view.Texture2DArray.PlaneSlice = 0;
                break;
            }
            return view;
        }
    } // namespace

    //==================================================================================================================
    void CommandList::clearTexture(Texture texture, const SubresourceRange& range, const ClearValue& value)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::clearTexture(list.core, texture, range, value);
        DeviceImpl& impl = d3d12::deviceOf(list);
        d3d12::ListState& state = list.state;
        // The payload is fixed from creation to retirement, so it is read with one acquire load and no lock (ADR 0020).
        const auto& payload = *static_cast<const d3d12::TexturePayload*>(list.core.device->pool<Texture>().native(texture));
        const QueueType type = list.core.type;
        // A Graphics list clears a render target or depth-stencil texture through the views it was created with; anything
        // else, and every texture a Compute list clears (Storage alone, VU-rojo-cmd-0061), takes an unordered-access clear.
        const bool graphics = type == QueueType::Graphics;
        const bool renderTarget = graphics && (payload.usage & TextureUsage::RenderTarget) != TextureUsage::None;
        const bool depthStencil = graphics && !renderTarget && (payload.usage & TextureUsage::DepthStencil) != TextureUsage::None;
        const ClearPath& path = renderTarget ? kRenderTargetPath : depthStencil ? kDepthStencilPath : kUnorderedAccessPath;
        // The subresources are in CopyDst (VU-rojo-cmd-0072): the clear moves them out of it and back, both planes of a
        // depth-stencil format together. A copy write runs in the copy scope alone, which the caller's barriers into and
        // out of CopyDst name too, so the bracket chains with them.
        const ClearPath copyDst = { d3d12::narrowSync(d3d12::copyStageSync(type), D3D12_BARRIER_ACCESS_COPY_DEST),
                                    D3D12_BARRIER_ACCESS_COPY_DEST,
                                    D3D12_BARRIER_LAYOUT_COPY_DEST };
        const D3D12_BARRIER_SUBRESOURCE_RANGE subresources = d3d12::rangeOf(payload, range);
        transition(list, payload.resource, subresources, copyDst, path);
        const uint32_t mipEnd = subresources.IndexOrFirstMipLevel + subresources.NumMipLevels;
        const uint32_t layerEnd = subresources.FirstArraySlice + subresources.NumArraySlices;
        const bool volume = payload.type == TextureType::Tex3D;
        const FLOAT color[4] = { value.color.r, value.color.g, value.color.b, value.color.a };
        if (renderTarget || depthStencil)
        {
            D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
            if (core::hasStencilPlane(payload.format))
            {
                flags |= D3D12_CLEAR_FLAG_STENCIL;
            }
            for (uint32_t mip = subresources.IndexOrFirstMipLevel; mip < mipEnd; ++mip)
            {
                // A 3D texture has one view per depth slice of each mip, any other texture one per layer.
                const uint32_t first = volume ? 0 : subresources.FirstArraySlice;
                const uint32_t end = volume ? slicesOf(payload.depth, mip) : layerEnd;
                for (uint32_t layer = first; layer < end; ++layer)
                {
                    const D3D12_CPU_DESCRIPTOR_HANDLE view = d3d12::targetView(payload, mip, layer);
                    if (renderTarget)
                    {
                        state.list->ClearRenderTargetView(view, color, 0, nullptr);
                    }
                    else
                    {
                        state.list->ClearDepthStencilView(view, flags, value.depth, value.stencil, 0, nullptr);
                    }
                    core::noteNativeCall(list.core);
                }
            }
        }
        else
        {
            for (uint32_t mip = subresources.IndexOrFirstMipLevel; mip < mipEnd; ++mip)
            {
                // The clear reads the view from the shader-visible heap the list set at acquisition and from its CPU-only twin.
                const d3d12::TransientSlot slot = impl.descriptors.claimTransient(state.frameSlot);
                const D3D12_UNORDERED_ACCESS_VIEW_DESC view = clearView(payload, mip, subresources);
                impl.device()->CreateUnorderedAccessView(payload.resource, nullptr, &view, slot.cpu);
                core::DeviceCounts::add(impl.core.counts().descriptorWrites);
                impl.device()->CreateUnorderedAccessView(payload.resource, nullptr, &view, slot.cpuTwin);
                core::DeviceCounts::add(impl.core.counts().descriptorWrites);
                state.list->ClearUnorderedAccessViewFloat(slot.gpu, slot.cpuTwin, payload.resource, color, 0, nullptr);
                core::noteNativeCall(list.core);
            }
        }
        transition(list, payload.resource, subresources, path, copyDst);
    }
} // namespace rojoRHI
