//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12RenderPasses.cpp
/// @brief Implements D3D12 render passes: native passes with their load, store and resolve operations, and their scopes.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Formats.h"
#include "Core/HandlePool.h"
#include "Core/RenderPasses.h"
#include "D3D12Barriers.h"
#include "D3D12Common.h"
#include "D3D12Formats.h"
#include "D3D12ListState.h"
#include "D3D12Scopes.h"
#include "D3D12Textures.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace rojoRHI {
    namespace {
        /// The blend factors every pass starts with (chapter 06).
        constexpr std::array<float, 4> kStartingBlendFactor = {};

        //==============================================================================================================
        /// Returns the beginning access `load` lowers to.
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE beginningOf(LoadOp load)
        {
            switch (load)
            {
            case LoadOp::Load: return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            case LoadOp::Clear: return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
            default: return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
            }
        }

        //==============================================================================================================
        /// Returns the ending access `store` lowers to when nothing resolves.
        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE endingOf(StoreOp store)
        {
            return store == StoreOp::Store ? D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE : D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
        }

        //==============================================================================================================
        /// Returns the beginning access of a depth or stencil plane loaded with `load`, which clears to `depth` and `stencil`
        /// in the depth-stencil view's `format`.
        D3D12_RENDER_PASS_BEGINNING_ACCESS depthBeginning(LoadOp load, DXGI_FORMAT format, float depth, uint8_t stencil)
        {
            D3D12_RENDER_PASS_BEGINNING_ACCESS access = {};
            access.Type = beginningOf(load);
            if (load == LoadOp::Clear)
            {
                access.Clear.ClearValue.Format = format;
                access.Clear.ClearValue.DepthStencil.Depth = depth;
                access.Clear.ClearValue.DepthStencil.Stencil = stencil;
            }
            return access;
        }

        //==============================================================================================================
        /// Returns the ending access of a depth or stencil plane stored with `store`.
        D3D12_RENDER_PASS_ENDING_ACCESS depthEnding(StoreOp store)
        {
            D3D12_RENDER_PASS_ENDING_ACCESS access = {};
            access.Type = endingOf(store);
            return access;
        }

        //==============================================================================================================
        /// Returns the barrier that moves mip 0 and layer 0 of `target`, a resolve target in `ColorAttachment`, which the
        /// caller's barriers left in `LAYOUT_RENDER_TARGET` (VU-rojo-cmd-0007), to `LAYOUT_RESOLVE_DEST`, where the pass's
        /// resolve writes it.
        D3D12_TEXTURE_BARRIER toResolveDest(const d3d12::TexturePayload& target)
        {
            D3D12_TEXTURE_BARRIER barrier = {};
            barrier.SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET;
            barrier.SyncAfter = D3D12_BARRIER_SYNC_RESOLVE;
            barrier.AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET;
            barrier.AccessAfter = D3D12_BARRIER_ACCESS_RESOLVE_DEST;
            barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
            barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_RESOLVE_DEST;
            barrier.pResource = target.resource;
            barrier.Subresources = d3d12::rangeOf(target, SubresourceRange{ .baseMip = 0, .mipCount = 1, .baseLayer = 0, .layerCount = 1 });
            barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
            return barrier;
        }

        //==============================================================================================================
        /// Turns `barrier`, made by `toResolveDest`, into the barrier that returns its subresource to `LAYOUT_RENDER_TARGET`
        /// once the resolve has written it, so the caller's next barrier finds the layout it named.
        void reverseToRenderTarget(D3D12_TEXTURE_BARRIER& barrier)
        {
            barrier.SyncBefore = D3D12_BARRIER_SYNC_RESOLVE;
            barrier.SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET;
            barrier.AccessBefore = D3D12_BARRIER_ACCESS_RESOLVE_DEST;
            barrier.AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET;
            barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_RESOLVE_DEST;
            barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        }

        //==============================================================================================================
        /// Records the first `count` barriers of `barriers` on `list` as one texture group of one native `Barrier` call.
        void recordResolveBarriers(CommandListImpl& list, const D3D12_TEXTURE_BARRIER* barriers, uint32_t count)
        {
            D3D12_BARRIER_GROUP group = {};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = count;
            group.pTextureBarriers = barriers;
            list.state.list->Barrier(1, &group);
            core::noteNativeCall(list.core);
            ++list.core.counts.nativeBarriers;
        }

        //==============================================================================================================
        /// Fills `target` with the render target of `color`, whose texture's payload is `payload` and whose resolve target's,
        /// when it names one, is `resolve`; a resolve takes the next of `pass`'s resolve parameters and barriers.
        void describeTarget(
            D3D12_RENDER_PASS_RENDER_TARGET_DESC& target,
            const ColorAttachment& color,
            const d3d12::TexturePayload& payload,
            const d3d12::TexturePayload* resolve,
            d3d12::PassStorage& pass
        )
        {
            // The format of the texture's render-target views, which clears and resolves name too.
            const DXGI_FORMAT format = d3d12::viewFormat(payload.format, Format::Undefined, TextureAspect::Color, TextureUsage::RenderTarget);
            target = {};
            target.cpuDescriptor = d3d12::targetView(payload, color.mip, color.layer);
            target.BeginningAccess.Type = beginningOf(color.load);
            if (color.load == LoadOp::Clear)
            {
                D3D12_CLEAR_VALUE& clear = target.BeginningAccess.Clear.ClearValue;
                clear.Format = format;
                clear.Color[0] = color.clear.r;
                clear.Color[1] = color.clear.g;
                clear.Color[2] = color.clear.b;
                clear.Color[3] = color.clear.a;
            }
            if (resolve == nullptr)
            {
                target.EndingAccess.Type = endingOf(color.store);
                return;
            }
            // The resolve averages the attachment's subresource into mip 0 and layer 0 of the resolve target (chapter 07).
            D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS& parameters = pass.resolves[pass.resolveCount];
            parameters.SrcSubresource = color.mip + color.layer * payload.mips;
            parameters.DstSubresource = 0;
            parameters.DstX = 0;
            parameters.DstY = 0;
            parameters.SrcRect =
                D3D12_RECT{ 0, 0, static_cast<LONG>(std::max(payload.width >> color.mip, 1u)), static_cast<LONG>(std::max(payload.height >> color.mip, 1u)) };
            target.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
            D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_PARAMETERS& resolved = target.EndingAccess.Resolve;
            resolved.pSrcResource = payload.resource;
            resolved.pDstResource = resolve->resource;
            resolved.SubresourceCount = 1;
            resolved.pSubresourceParameters = &parameters;
            resolved.Format = format;
            resolved.ResolveMode = D3D12_RESOLVE_MODE_AVERAGE;
            // With DontCare the attachment's samples are undefined after the pass, so the resolve may drop them (chapter 07).
            resolved.PreserveResolveSource = color.store == StoreOp::Store ? TRUE : FALSE;
            pass.resolveBarriers[pass.resolveCount] = toResolveDest(*resolve);
            ++pass.resolveCount;
        }

        //==============================================================================================================
        /// Fills `depth` with the depth-stencil attachment `attachment`, whose texture's payload is `payload`. A format without
        /// a stencil plane leaves stencil untouched, whatever the stencil operations say.
        void describeDepth(D3D12_RENDER_PASS_DEPTH_STENCIL_DESC& depth, const DepthAttachment& attachment, const d3d12::TexturePayload& payload)
        {
            const DXGI_FORMAT format = d3d12::dsvFormat(payload.format);
            depth = {};
            depth.cpuDescriptor = d3d12::targetView(payload, attachment.mip, attachment.layer);
            depth.DepthBeginningAccess = depthBeginning(attachment.load, format, attachment.clearDepth, attachment.clearStencil);
            depth.DepthEndingAccess = depthEnding(attachment.store);
            if (core::hasStencilPlane(payload.format))
            {
                depth.StencilBeginningAccess = depthBeginning(attachment.stencilLoad, format, attachment.clearDepth, attachment.clearStencil);
                depth.StencilEndingAccess = depthEnding(attachment.stencilStore);
            }
            else
            {
                depth.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
                depth.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
            }
        }
    } // namespace

    //==================================================================================================================
    void CommandList::beginRenderPass(const RenderPassDesc& desc)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        // The core checks the pass and records its scope last in the scope log.
        core::beginPass(list.core, desc);
        d3d12::beginNativeScope(list);
        d3d12::ListState& state = list.state;
        d3d12::PassStorage& pass = state.pass;
        // Payloads are fixed while their textures live, so each is read with one acquire load and no lock (ADR 0020).
        const auto& texturePool = list.core.device->pool<Texture>();
        pass.targetCount = static_cast<uint32_t>(desc.colors.size());
        pass.resolveCount = 0;
        for (uint32_t i = 0; i < pass.targetCount; ++i)
        {
            const ColorAttachment& color = desc.colors[i];
            const auto& payload = *static_cast<const d3d12::TexturePayload*>(texturePool.native(color.texture));
            const auto* resolve = color.resolve.bits == 0 ? nullptr : static_cast<const d3d12::TexturePayload*>(texturePool.native(color.resolve));
            describeTarget(pass.targets[i], color, payload, resolve, pass);
        }
        // Every resolve target moves to the resolve's layout in one call; EndRenderPass moves it back.
        if (pass.resolveCount > 0)
        {
            recordResolveBarriers(list, pass.resolveBarriers.data(), pass.resolveCount);
        }
        pass.hasDepth = desc.depth.texture.bits != 0;
        if (pass.hasDepth)
        {
            describeDepth(pass.depth, desc.depth, *static_cast<const d3d12::TexturePayload*>(texturePool.native(desc.depth.texture)));
        }
        state.list->BeginRenderPass(pass.targetCount, pass.targets.data(), pass.hasDepth ? &pass.depth : nullptr, D3D12_RENDER_PASS_FLAG_NONE);
        core::noteNativeCall(list.core);
        ++list.core.counts.encodersOpened;
        // The native list keeps its dynamic state across passes, so the starting values go out only where it holds another or
        // an unknown value; the depth bias is recorded, and sent by the pipeline that enables it.
        d3d12::DynamicState& dynamic = state.dynamic;
        if (dynamic.stencilReference != uint8_t{ 0 })
        {
            state.list->OMSetStencilRef(0);
            core::noteNativeCall(list.core);
            dynamic.stencilReference = uint8_t{ 0 };
        }
        if (dynamic.blendFactor != kStartingBlendFactor)
        {
            state.list->OMSetBlendFactor(kStartingBlendFactor.data());
            core::noteNativeCall(list.core);
            dynamic.blendFactor = kStartingBlendFactor;
        }
        dynamic.depthBias = {};
    }

    //==================================================================================================================
    void CommandList::endRenderPass()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::endPass(list.core);
        d3d12::ListState& state = list.state;
        state.list->EndRenderPass();
        core::noteNativeCall(list.core);
        // The resolves have written their targets: each returns to the render-target layout the caller's barriers name.
        d3d12::PassStorage& pass = state.pass;
        if (pass.resolveCount > 0)
        {
            for (uint32_t i = 0; i < pass.resolveCount; ++i)
            {
                reverseToRenderTarget(pass.resolveBarriers[i]);
            }
            recordResolveBarriers(list, pass.resolveBarriers.data(), pass.resolveCount);
        }
        d3d12::endNativeScope(list);
    }
} // namespace rojoRHI
