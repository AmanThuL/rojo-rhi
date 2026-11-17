//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12RootSignature.h
/// @brief Declares the D3D12 device's global and kernel root signatures and its four command signatures.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <cstddef>
#include <span>
#include <vector>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
    // Created once with the device and fixed for its life.
    class RootSignatures
    {
      public:
        Result<void> create(DeviceImpl& impl);

        // Graphics and compute pipelines share it: the root SRV and the draw-record constant.
        ID3D12RootSignature* global() const;

        ID3D12RootSignature* kernel() const;

        ID3D12CommandSignature* draw() const;

        ID3D12CommandSignature* drawIndexed() const;

        ID3D12CommandSignature* mesh() const;

        ID3D12CommandSignature* dispatch() const;

        // Kept for the device's life.
        std::span<const std::byte> serializedGlobal() const;

      private:
        Com<ID3D12RootSignature> m_global;
        Com<ID3D12RootSignature> m_kernel;
        Com<ID3D12CommandSignature> m_draw;
        Com<ID3D12CommandSignature> m_drawIndexed;
        Com<ID3D12CommandSignature> m_mesh;
        Com<ID3D12CommandSignature> m_dispatch;
        std::vector<std::byte> m_serializedGlobal;
    };
} // namespace rojoRHI::d3d12
