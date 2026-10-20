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
    /// depth-stencil views, fixed from creation to retirement, when the retire hook frees it.
    struct TexturePayload
    {
        ID3D12Resource* resource = nullptr;               ///< The native texture, one reference owned.
        Com<ID3D12DescriptorHeap> targets;                ///< One RTV or DSV per target subresource; null for neither.
        uint32_t targetCount = 0;                         ///< The descriptors of `targets`.
        DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN; ///< The format the resource was created with.
    };

    /// Returns the CPU handle of the render-target or depth-stencil view of `payload`'s subresource at `mip` and `layer`, the
    /// depth slice for a 3D texture.
    D3D12_CPU_DESCRIPTOR_HANDLE targetView(const TexturePayload& payload, uint32_t mip, uint32_t layer);

    /// Releases the target heap, the resource and the payload of a retiring texture record. `context` is the `DeviceImpl`; a
    /// null payload is skipped. It runs under the texture pool's lock.
    void retireTexture(void* context, core::TextureRecord& record);
} // namespace rojoRHI::d3d12
