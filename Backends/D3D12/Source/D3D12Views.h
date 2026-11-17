//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Views.h
/// @brief Declares the D3D12 descriptions of texture views and the retire hook of view slots, which createDevice registers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"
#include "D3D12Common.h"

namespace rojoRHI::d3d12 {
    // The component mapping of a stencil view: the stencil plane's value, which `DXGI_FORMAT_X32_TYPELESS_G8X24_UINT` holds
    // in its second component, is read in the first, with 0 in the second and third and 1 in the fourth.
    inline constexpr UINT kStencilMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1
    );

    // A single-layer `Tex1D` or `Tex2D` view of layer 0 takes the non-array dimension, which views slice 0 of any
    // resource; of a later layer of a multi-layer resource, the array dimension with one layer. A `Cube` view of a
    // later cube is a cube array of one cube. A stencil view reads plane 1 of a depth-stencil format through
    // `kStencilMapping`.
    D3D12_SHADER_RESOURCE_VIEW_DESC shaderResourceDesc(const core::TextureRecord& texture, const TextureViewDesc& desc);

    // `desc` is a `Storage` view of one mip. A UAV has no cube dimension, so a cube view is the 2D array of its faces;
    // an sRGB format is written through its linear partner.
    D3D12_UNORDERED_ACCESS_VIEW_DESC unorderedAccessDesc(const core::TextureRecord& texture, const TextureViewDesc& desc);

    // Releases the texture reference a view slot holds; `native` is that `ID3D12Resource`, `context` the DeviceImpl.
    void retireView(void* context, void* native);
} // namespace rojoRHI::d3d12
