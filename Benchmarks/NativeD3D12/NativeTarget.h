//----------------------------------------------------------------------------------------------------------------------
/// @file NativeTarget.h
/// @brief Declares the native arm's D3D12 render targets of the drawing CPU workloads, their passes, pipelines and readback.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/CpuRoots.h"
#include "NativeD3D12/NativeContext.h"

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    // One render pass over a target: its color attachment, cleared to zero and stored, and its depth attachment, cleared to
    // 0 and stored, when the target has one.
    struct NativePass
    {
        D3D12_RENDER_PASS_RENDER_TARGET_DESC color{};
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depth{};
        bool hasDepth = false;
    };

    // The fixed-function state of a target pipeline: counter-clockwise front faces, the 256x256 RGBA8Unorm color target and,
    // when `depth` is set, a D32Float depth target.
    struct NativeTargetState
    {
        D3D12_CULL_MODE cull = D3D12_CULL_MODE_NONE;
        bool depth = false;
        bool depthTest = false;
        bool depthWrite = false;
        D3D12_COMPARISON_FUNC depthCompare = D3D12_COMPARISON_FUNC_ALWAYS;
    };

    // Per frame slot, a 256x256 RGBA8Unorm color texture and, when asked, a D32Float depth texture, both created in their
    // attachment layouts, with a render-target and a depth-stencil view of each and a render pass that clears color to zero
    // and depth to 0 and stores both. Frame f renders to slot f mod 3 only, as the RojoRHI arm does.
    class NativeTarget
    {
      public:
        // The whole target, set once per list before the repetitions.
        static constexpr D3D12_VIEWPORT kViewport{ 0.0f, 0.0f, 256.0f, 256.0f, 0.0f, 1.0f };
        static constexpr D3D12_RECT kScissor{ 0, 0, 256, 256 };

        // Creates the textures, views and passes through the context, before prepareFrames.
        std::expected<void, std::string> create(NativeContext& context, std::string_view label, bool depth);

        const NativePass& pass(uint32_t slot) const;
        // The barrier between two passes over the slot's color texture: render target to render target, the layout staying.
        D3D12_TEXTURE_BARRIER passBarrier(uint32_t slot) const;

        // A pipeline drawing into the color target with a vertex and a pixel entry of one DXIL library.
        static std::expected<ID3D12PipelineState*, std::string> pipeline(
            NativeContext& context,
            std::string_view library,
            std::string_view vertex,
            std::string_view pixel,
            std::string_view label,
            const NativeTargetState& state
        );

        // Outside every measurement: "target", then "depth" when the slot has one, each with tight rows.
        std::expected<std::vector<Output>, std::string> read(NativeContext& context, uint32_t slot) const;

      private:
        std::array<ID3D12Resource*, NativeContext::kFrameSlots> m_color{};
        std::array<ID3D12Resource*, NativeContext::kFrameSlots> m_depth{};
        std::array<NativePass, NativeContext::kFrameSlots> m_passes{};
    };

    // Resets list `list` of the slot onto its allocator: one counted host call.
    std::expected<void, std::string> resetList(NativeContext& context, const NativeContext::Slot& slot, uint32_t list);
    // Closes a list: one counted record call.
    std::expected<void, std::string> closeList(NativeContext& context, ID3D12GraphicsCommandList9* list);

    // Outside every measurement: one global barrier from the writes a workload left in the buffer to a copy read, then the
    // buffer's bytes through the context.
    std::expected<std::vector<std::byte>, std::string> readBufferAfter(
        NativeContext& context, ID3D12Resource* source, uint64_t offset, uint64_t size, D3D12_BARRIER_SYNC syncBefore, D3D12_BARRIER_ACCESS accessBefore
    );
} // namespace rojoRHI::bench
