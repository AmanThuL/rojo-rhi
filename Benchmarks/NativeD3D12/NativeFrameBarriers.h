//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrameBarriers.h
/// @brief Declares the native lowering of every barrier batch of the composed frame on D3D12: one enhanced Barrier call per batch.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/FrameLayout.h"
#include "NativeD3D12/NativeContext.h"

#include <array>
#include <cstdint>
#include <span>

namespace rojoRHI::bench {
    // The barrier batches of the composed frame in recording order: the batch before each pass that has one, the batch
    // inside the `ao` pass, and the batches that close the `gbuffer` and `ao` lists.
    enum class FrameBatch : uint8_t
    {
        Geometry,     ///< Before pass 2, opening the gbuffer list.
        GbufferClose, ///< After pass 2, closing the gbuffer list.
        AoStart,      ///< Before pass 3, opening the ao list.
        AoRaw,        ///< Inside pass 3, between the occlusion and blur dispatches.
        AoClose,      ///< After pass 3, closing the ao list.
        Shadows,      ///< Before pass 4, opening the shadows list.
        Lighting,     ///< Before pass 7, opening the lighting list.
        Forward,      ///< Before pass 8.
        BloomDown0,   ///< Before pass 9.
        BloomDown1,   ///< Before pass 10.
        BloomUp1,     ///< Before pass 11.
        BloomUp0,     ///< Before pass 12.
        ToneMap,      ///< Before pass 13.
        Readback,     ///< Before pass 14.
    };

    inline constexpr uint32_t kFrameBatchCount = 14;
    // The most textures one batch names: the geometry pass's three color targets and its depth.
    inline constexpr uint32_t kMaxBatchTextures = 4;

    // One texture barrier of a batch as chapter 09's D3D12 rows lower it, the resource aside. It covers every subresource
    // of the texture, and a barrier from LAYOUT_UNDEFINED discards.
    struct NativeTextureBarrier
    {
        FrameTexture texture;
        D3D12_BARRIER_SYNC syncBefore;
        D3D12_BARRIER_SYNC syncAfter;
        D3D12_BARRIER_ACCESS accessBefore;
        D3D12_BARRIER_ACCESS accessAfter;
        D3D12_BARRIER_LAYOUT layoutBefore;
        D3D12_BARRIER_LAYOUT layoutAfter;
    };

    // One batch: its texture barriers, the first `count` of `textures`, recorded as one texture group of one Barrier call.
    struct NativeBatch
    {
        FrameBatch batch;
        uint32_t count;
        std::array<NativeTextureBarrier, kMaxBatchTextures> textures;
    };

    // The lowering of every batch, in FrameBatch order; NativeFrameBarriers.cpp states how each row follows from the batch.
    extern const std::array<NativeBatch, kFrameBatchCount> kFrameBarriers;

    /// The lowering of one batch.
    const NativeBatch& nativeBatch(FrameBatch batch);

    /// The layout a frame leaves `texture` in: the layout after the last batch that names it.
    D3D12_BARRIER_LAYOUT frameEndLayout(FrameTexture texture);

    /// The D3D12 texture barrier of `row` over `resource`, a texture of `layers` array layers and one mip and plane.
    D3D12_TEXTURE_BARRIER textureBarrier(const NativeTextureBarrier& row, ID3D12Resource* resource, uint32_t layers);

    /// Records `barriers` as one texture group of one Barrier call on `list` and counts it.
    void emitBarriers(ID3D12GraphicsCommandList9& list, std::span<const D3D12_TEXTURE_BARRIER> barriers, NativeCalls& calls);
} // namespace rojoRHI::bench
