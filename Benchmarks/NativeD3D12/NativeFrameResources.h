//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrameResources.h
/// @brief Declares the native arm's composed-frame resources on D3D12: textures, views, buffers, passes, pipelines and their dumps.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/FrameLayout.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeIndirect.h"

#include <rojoRHI/RHI.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace rojoRHI::bench {
    // The compute pipelines and the render pipelines of the composed frame, each on the context's global root signature.
    struct NativeFramePipelines
    {
        ID3D12PipelineState* cull = nullptr;
        ID3D12PipelineState* ao = nullptr;
        ID3D12PipelineState* blur = nullptr;
        ID3D12PipelineState* lighting = nullptr;
        ID3D12PipelineState* geometry = nullptr;
        ID3D12PipelineState* shadow = nullptr;
        ID3D12PipelineState* forward = nullptr;
        ID3D12PipelineState* bloomDown = nullptr;
        ID3D12PipelineState* bloomUp = nullptr;
        ID3D12PipelineState* toneMap = nullptr;
    };

    // One render pass of kRenderPasses: its attachments with their beginning and ending accesses, and the viewport and
    // scissor that cover its target.
    struct NativeFramePass
    {
        uint32_t colorCount = 0;
        std::array<D3D12_RENDER_PASS_RENDER_TARGET_DESC, 3> colors{};
        bool hasDepth = false;
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depth{};
        D3D12_VIEWPORT viewport{};
        D3D12_RECT scissor{};
    };

    /// Builds every pipeline of the composed frame from the DXIL the Slang rule writes: the four kernels, and a graphics
    /// pipeline per pass kind with the attachments, rasterisation, depth test and blend of its render pass. The error names
    /// the entry that failed.
    std::expected<NativeFramePipelines, std::string> createFramePipelines(NativeContext& context);

    /// The index in kRenderPasses of the render pass that is pass `pass` of kPasses, or kRenderPassCount for a pass that
    /// is no render pass.
    constexpr uint32_t renderPassIndex(uint32_t pass)
    {
        for (uint32_t index = 0; index < kRenderPassCount; ++index)
        {
            if (kRenderPasses[index].pass == pass)
            {
                return index;
            }
        }
        return kRenderPassCount;
    }

    // Every texture and buffer FrameLayout.h lists, created through the context: the textures in LAYOUT_COMMON (every
    // frame's first barrier of each discards it from LAYOUT_UNDEFINED), the depth textures typeless so the kernels sample
    // their depth; the scene's buffers uploaded once; records[s] DEFAULT with a raw SRV and UAV, which the module's Addr
    // names; readback[s] READBACK. Views sit in the context's shader-visible heap: a sampled view of every Sampled
    // texture (Tex2DArray over the three layers of `shadow`, R32_FLOAT for the depth textures), a storage view of aoRaw,
    // ao and hdr, and the frame's one sampler. Render-target views of every color target and depth-stencil views of
    // `depth` and of each layer of `shadow` sit in CPU-only heaps of their own. The camera's view-projection goes into
    // camera[s], an UPLOAD buffer with a raw SRV per slot, since the forward roots name it by an Addr.
    class NativeFrameResources
    {
      public:
        explicit NativeFrameResources(NativeContext& context);

        // Setup, before NativeContext::prepareFrames.
        std::expected<void, std::string> create();

        ID3D12Resource* texture(FrameTexture which) const;
        uint32_t view(FrameTexture which) const;        ///< The sampled view.
        uint32_t storageView(FrameTexture which) const; ///< The storage view of aoRaw, ao or hdr.
        uint32_t sampler() const;
        Addr instances() const;
        Addr vertices() const;
        const D3D12_INDEX_BUFFER_VIEW& indices() const;
        ID3D12Resource* records(uint32_t slot) const;
        Addr recordsAddr(uint32_t slot) const;
        ID3D12Resource* readback(uint32_t slot) const;
        Addr camera(uint32_t slot) const;
        std::byte* cameraData(uint32_t slot) const;
        // Pass `which` of kPasses, which is a render pass.
        const NativeFramePass& pass(uint32_t which) const;
        const NativeFramePipelines& pipelines() const;
        NativeIndirect& indirect();

        // Outside every measurement, after the last frame completed: every texture output read from the layout the frame
        // leaves it in, with tight rows, packRecords of records[slot] and the row in readback[slot], in kOutputs order.
        std::expected<std::vector<Output>, std::string> readOutputs(uint32_t slot);

      private:
        std::expected<void, std::string> createTextures();
        std::expected<void, std::string> createTargets();
        std::expected<void, std::string> createBuffers();
        void createPasses();

        NativeContext& m_context;
        NativeIndirect m_indirect;
        NativeFramePipelines m_pipelines;
        std::array<ID3D12Resource*, kTextureCount> m_textures{};
        std::array<uint32_t, kTextureCount> m_views{};
        std::array<uint32_t, kTextureCount> m_storageViews{};
        uint32_t m_sampler = 0;
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kTextureCount> m_renderTargets{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1 + kCascadeCount> m_depthTargets{}; ///< `depth`, then the layers of `shadow`.
        uint32_t m_instancesView = 0;
        uint32_t m_verticesView = 0;
        D3D12_INDEX_BUFFER_VIEW m_indices{};
        std::array<ID3D12Resource*, kFrameSlots> m_records{};
        std::array<uint32_t, kFrameSlots> m_recordsViews{};
        std::array<ID3D12Resource*, kFrameSlots> m_readback{};
        std::array<uint32_t, kFrameSlots> m_cameraViews{};
        std::array<std::byte*, kFrameSlots> m_cameraData{};
        std::array<NativeFramePass, kRenderPassCount> m_passes{};
    };
} // namespace rojoRHI::bench
