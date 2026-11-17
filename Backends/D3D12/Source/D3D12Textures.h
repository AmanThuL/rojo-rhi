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
    // Fixed from creation to retirement, when the retire hook frees it: recording reads it without a lock.
    struct TexturePayload
    {
        ID3D12Resource* resource = nullptr; // One reference owned.
        Com<ID3D12DescriptorHeap> targets;  // One RTV or DSV per target subresource; null for neither.
        uint32_t targetCount = 0;
        DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
        D3D12_CPU_DESCRIPTOR_HANDLE targetStart = {};
        uint32_t targetIncrement = 0;
        uint32_t targetLayers = 1; // Faces for cube types; 1 for a 3D texture.
        uint32_t targetDepth = 1;  // Depth slices of mip 0 for a 3D texture; 1 otherwise.
        Format format = Format::Undefined;
        TextureType type = TextureType::Tex2D;
        TextureUsage usage = TextureUsage::None;
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t mips = 1;
        uint32_t layers = 1; // Faces for cube types; 1 for a 3D texture.
        uint32_t depth = 1;  // Depth slices of mip 0 for a 3D texture; 1 otherwise.
        uint32_t samples = 1;
        // Subresource 0's footprint format as `GetCopyableFootprints1` reports it, read once at creation so a copy
        // never queries the device while recording.
        DXGI_FORMAT copyFormat = DXGI_FORMAT_UNKNOWN;
    };

    // `layer` is the depth slice for a 3D texture. The views are ordered by mip, then by layer or depth slice within a
    // mip. A layer or slice the mip does not have, or a texture with no target views, is an internal error.
    D3D12_CPU_DESCRIPTOR_HANDLE targetView(const TexturePayload& payload, uint32_t mip, uint32_t layer);

    // Counts one `nativeCreateCalls` for the target heap. `context` is the DeviceImpl; a null payload, and the borrowed
    // payload of a swapchain image, are skipped. Runs under the texture pool's lock.
    void retireTexture(void* context, core::TextureRecord& record);
} // namespace rojoRHI::d3d12
