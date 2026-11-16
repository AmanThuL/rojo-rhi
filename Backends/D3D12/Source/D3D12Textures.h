//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Textures.h
/// @brief Declares the D3D12 texture payload, its render-target and depth-stencil views, and the texture retire hook.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"
#include "D3D12Common.h"

#include <cstdint>

namespace rojoRHI::d3d12 {
    /// The payload of a texture record: the resource, one reference owned, and the CPU-only heap of its render-target or
    /// depth-stencil views, fixed from creation to retirement, when the retire hook frees it. Recording reads it without a
    /// lock, so nothing in it changes while the texture lives.
    struct TexturePayload
    {
        ID3D12Resource* resource = nullptr;               ///< The native texture, one reference owned.
        Com<ID3D12DescriptorHeap> targets;                ///< One RTV or DSV per target subresource; null for neither.
        uint32_t targetCount = 0;                         ///< The descriptors of `targets`.
        DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN; ///< The format the resource was created with.
        D3D12_CPU_DESCRIPTOR_HANDLE targetStart = {};     ///< The CPU handle of the first descriptor of `targets`.
        uint32_t targetIncrement = 0;                     ///< Bytes between two descriptors of `targets`.
        uint32_t targetLayers = 1;                        ///< Array layers, faces for cube types; 1 for a 3D texture.
        uint32_t targetDepth = 1;                         ///< Depth slices of mip 0 for a 3D texture; 1 otherwise.
        Format format = Format::Undefined;                ///< The texture's format.
        TextureType type = TextureType::Tex2D;            ///< The texture's type.
        TextureUsage usage = TextureUsage::None;          ///< The texture's usage.
        uint32_t width = 1;                               ///< Width of mip 0 in texels.
        uint32_t height = 1;                              ///< Height of mip 0 in texels.
        uint32_t mips = 1;                                ///< Mip levels.
        uint32_t layers = 1;                              ///< Array layers, faces for cube types; 1 for a 3D texture.
        uint32_t depth = 1;                               ///< Depth slices of mip 0 for a 3D texture; 1 otherwise.
        uint32_t samples = 1;                             ///< Samples per texel.
        /// The format of subresource 0's footprint as `GetCopyableFootprints1` reports it, read once at creation, so a copy
        /// names it without querying the device while recording.
        DXGI_FORMAT copyFormat = DXGI_FORMAT_UNKNOWN;
    };

    /// Returns the CPU handle of the render-target or depth-stencil view of `payload`'s subresource at `mip` and `layer`, the
    /// depth slice for a 3D texture. The views are ordered by mip, then by layer or depth slice within a mip. A layer or slice
    /// the mip does not have, or a texture with no target views, is an internal error.
    D3D12_CPU_DESCRIPTOR_HANDLE targetView(const TexturePayload& payload, uint32_t mip, uint32_t layer);

    /// Releases the target heap, counting one `nativeCreateCalls` for it, the resource and the payload of a retiring texture
    /// record. `context` is the `DeviceImpl`; a null payload, and the borrowed payload of a swapchain image, are skipped. It
    /// runs under the texture pool's lock.
    void retireTexture(void* context, core::TextureRecord& record);
} // namespace rojoRHI::d3d12
