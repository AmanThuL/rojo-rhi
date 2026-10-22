//----------------------------------------------------------------------------------------------------------------------
/// @file NativeTarget.cpp
/// @brief Implements the native arm's D3D12 render targets, their passes, the target pipeline, list helpers and readback.
//----------------------------------------------------------------------------------------------------------------------
#include "NativeD3D12/NativeTarget.h"

#include "Common/CpuRoots.h"

#include <format>
#include <utility>

namespace rojoRHI::bench {
    namespace {
        //==============================================================================================================
        std::expected<ID3D12Resource*, std::string> targetTexture(
            NativeContext& context, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_BARRIER_LAYOUT layout, std::string_view label
        )
        {
            D3D12_RESOURCE_DESC1 desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = kTargetSize;
            desc.Height = kTargetSize;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = flags;
            return context.texture(desc, layout, label);
        }

        //==============================================================================================================
        // A render pass attachment that keeps what the pass wrote.
        D3D12_RENDER_PASS_ENDING_ACCESS preserved()
        {
            D3D12_RENDER_PASS_ENDING_ACCESS access{};
            access.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            return access;
        }
    } // namespace

    //==================================================================================================================
    // Both views of a slot sit at its index in their CPU-only heaps, which hold nothing else.
    std::expected<void, std::string> NativeTarget::create(NativeContext& context, std::string_view label, bool depth)
    {
        ID3D12Device12* device = context.device();
        NativeCalls& calls = context.calls();
        const std::expected<ID3D12DescriptorHeap*, std::string> colorHeap =
            context.descriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NativeContext::kFrameSlots, false, std::format("{}.rtv", label));
        if (!colorHeap)
        {
            return std::unexpected(colorHeap.error());
        }
        const UINT colorStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const D3D12_CPU_DESCRIPTOR_HANDLE colorStart = (*colorHeap)->GetCPUDescriptorHandleForHeapStart();
        calls.host += 2;

        UINT depthStep = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE depthStart{};
        if (depth)
        {
            const std::expected<ID3D12DescriptorHeap*, std::string> depthHeap =
                context.descriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, NativeContext::kFrameSlots, false, std::format("{}.dsv", label));
            if (!depthHeap)
            {
                return std::unexpected(depthHeap.error());
            }
            depthStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            depthStart = (*depthHeap)->GetCPUDescriptorHandleForHeapStart();
            calls.host += 2;
        }

        for (uint32_t slot = 0; slot < NativeContext::kFrameSlots; ++slot)
        {
            const std::expected<ID3D12Resource*, std::string> color = targetTexture(
                context,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                std::format("{}.target.{}", label, slot)
            );
            if (!color)
            {
                return std::unexpected(color.error());
            }
            m_color[slot] = *color;
            D3D12_RENDER_TARGET_VIEW_DESC colorView{};
            colorView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            colorView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            const D3D12_CPU_DESCRIPTOR_HANDLE colorHandle{ colorStart.ptr + SIZE_T{ slot } * colorStep };
            device->CreateRenderTargetView(m_color[slot], &colorView, colorHandle);
            ++calls.host;

            NativePass& attachments = m_passes[slot];
            attachments.color.cpuDescriptor = colorHandle;
            attachments.color.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
            attachments.color.BeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            attachments.color.EndingAccess = preserved();

            if (depth)
            {
                const std::expected<ID3D12Resource*, std::string> depthTexture = targetTexture(
                    context,
                    DXGI_FORMAT_D32_FLOAT,
                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                    D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
                    std::format("{}.depth.{}", label, slot)
                );
                if (!depthTexture)
                {
                    return std::unexpected(depthTexture.error());
                }
                m_depth[slot] = *depthTexture;
                D3D12_DEPTH_STENCIL_VIEW_DESC depthView{};
                depthView.Format = DXGI_FORMAT_D32_FLOAT;
                depthView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                const D3D12_CPU_DESCRIPTOR_HANDLE depthHandle{ depthStart.ptr + SIZE_T{ slot } * depthStep };
                device->CreateDepthStencilView(m_depth[slot], &depthView, depthHandle);
                ++calls.host;

                attachments.hasDepth = true;
                attachments.depth.cpuDescriptor = depthHandle;
                attachments.depth.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                attachments.depth.DepthBeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_D32_FLOAT;
                attachments.depth.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth = 0.0f;
                attachments.depth.DepthEndingAccess = preserved();
                attachments.depth.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
                attachments.depth.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
            }
        }
        return {};
    }

    //==================================================================================================================
    const NativePass& NativeTarget::pass(uint32_t slot) const
    {
        return m_passes[slot];
    }

    //==================================================================================================================
    D3D12_TEXTURE_BARRIER NativeTarget::passBarrier(uint32_t slot) const
    {
        D3D12_TEXTURE_BARRIER barrier{};
        barrier.SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET;
        barrier.SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET;
        barrier.AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET;
        barrier.AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET;
        barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        barrier.pResource = m_color[slot];
        // NumMipLevels 0 makes IndexOrFirstMipLevel a subresource index.
        barrier.Subresources.IndexOrFirstMipLevel = 0;
        barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
        return barrier;
    }

    //==================================================================================================================
    std::expected<ID3D12PipelineState*, std::string> NativeTarget::pipeline(
        NativeContext& context,
        std::string_view library,
        std::string_view vertex,
        std::string_view pixel,
        std::string_view label,
        const NativeTargetState& state
    )
    {
        const std::expected<D3D12_SHADER_BYTECODE, std::string> vertexShader = context.library(library, vertex);
        if (!vertexShader)
        {
            return std::unexpected(vertexShader.error());
        }
        const std::expected<D3D12_SHADER_BYTECODE, std::string> pixelShader = context.library(library, pixel);
        if (!pixelShader)
        {
            return std::unexpected(pixelShader.error());
        }
        NativeGraphicsDesc desc;
        desc.vertex = *vertexShader;
        desc.pixel = *pixelShader;
        desc.rasterizer.CullMode = state.cull;
        desc.rasterizer.FrontCounterClockwise = TRUE;
        desc.depthStencil.DepthEnable = state.depthTest ? TRUE : FALSE;
        desc.depthStencil.DepthWriteMask = state.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.depthStencil.DepthFunc = state.depthCompare;
        desc.colorFormats = { DXGI_FORMAT_R8G8B8A8_UNORM };
        desc.depthFormat = state.depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        return context.graphicsPipeline(desc, label);
    }

    //==================================================================================================================
    // The context reads each texture with tight rows, so a pitch of 256 bytes cannot reach the outputs; the size check
    // names a reader that stopped doing so.
    std::expected<std::vector<Output>, std::string> NativeTarget::read(NativeContext& context, uint32_t slot) const
    {
        std::vector<Output> outputs;
        std::expected<std::vector<std::byte>, std::string> color = context.readTexture(m_color[slot], D3D12_BARRIER_LAYOUT_RENDER_TARGET);
        if (!color)
        {
            return std::unexpected("target readback: " + color.error());
        }
        if (color->size() != kTargetBytes)
        {
            return std::unexpected(std::format("target readback: the target read {} bytes, tight rows hold {}", color->size(), kTargetBytes));
        }
        outputs.push_back(Output{ "target", std::move(*color) });
        if (m_depth[slot] != nullptr)
        {
            std::expected<std::vector<std::byte>, std::string> depth = context.readTexture(m_depth[slot], D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE);
            if (!depth)
            {
                return std::unexpected("depth readback: " + depth.error());
            }
            if (depth->size() != kTargetBytes)
            {
                return std::unexpected(std::format("depth readback: the depth read {} bytes, tight rows hold {}", depth->size(), kTargetBytes));
            }
            outputs.push_back(Output{ "depth", std::move(*depth) });
        }
        return outputs;
    }

    //==================================================================================================================
    std::expected<void, std::string> resetList(NativeContext& context, const NativeContext::Slot& slot, uint32_t list)
    {
        const HRESULT reset = slot.lists[list]->Reset(slot.allocators[list], nullptr);
        ++context.calls().host;
        if (FAILED(reset))
        {
            return std::unexpected(std::format("ID3D12GraphicsCommandList::Reset of list {} failed (0x{:08X})", list, static_cast<uint32_t>(reset)));
        }
        context.startTimestamps(slot.lists[list]);
        return {};
    }

    //==================================================================================================================
    std::expected<void, std::string> closeList(NativeContext& context, ID3D12GraphicsCommandList9* list)
    {
        context.endTimestamps(list);
        const HRESULT closed = list->Close();
        ++context.calls().record;
        if (FAILED(closed))
        {
            return std::unexpected(std::format("ID3D12GraphicsCommandList::Close failed (0x{:08X})", static_cast<uint32_t>(closed)));
        }
        return {};
    }

    //==================================================================================================================
    std::expected<std::vector<std::byte>, std::string> readBufferAfter(
        NativeContext& context, ID3D12Resource* source, uint64_t offset, uint64_t size, D3D12_BARRIER_SYNC syncBefore, D3D12_BARRIER_ACCESS accessBefore
    )
    {
        NativeCalls& calls = context.calls();
        const std::expected<void, std::string> ordered = context.runSetup([&](ID3D12GraphicsCommandList9& list) {
            D3D12_GLOBAL_BARRIER barrier{};
            barrier.SyncBefore = syncBefore;
            barrier.SyncAfter = D3D12_BARRIER_SYNC_COPY;
            barrier.AccessBefore = accessBefore;
            barrier.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_GLOBAL;
            group.NumBarriers = 1;
            group.pGlobalBarriers = &barrier;
            list.Barrier(1, &group);
            ++calls.record;
            ++calls.barriers;
        });
        if (!ordered)
        {
            return std::unexpected("readback barrier: " + ordered.error());
        }
        return context.readBuffer(source, offset, size);
    }
} // namespace rojoRHI::bench
