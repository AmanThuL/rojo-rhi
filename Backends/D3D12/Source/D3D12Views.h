//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Views.h
/// @brief Declares the retire hook of D3D12 view slots, which createDevice registers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

namespace rojoRHI::d3d12 {
    /// Releases the texture reference a view slot holds, `native` being its `ID3D12Resource`. `context` is the `DeviceImpl`.
    void retireView(void* context, void* native);
} // namespace rojoRHI::d3d12
