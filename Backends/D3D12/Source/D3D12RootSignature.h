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
    /// The root signatures every pipeline of a device shares and the command signatures of its indirect commands, created
    /// once with the device and fixed for its life.
    class RootSignatures
    {
      public:
        /// Creates every signature on `impl`'s device.
        Result<void> create(DeviceImpl& impl);

        /// Returns the global root signature of graphics and compute pipelines: the root SRV and the draw-record constant.
        ID3D12RootSignature* global() const;

        /// Returns the root signature of the backend's own kernels.
        ID3D12RootSignature* kernel() const;

        /// Returns the command signature of an indirect draw.
        ID3D12CommandSignature* draw() const;

        /// Returns the command signature of an indirect indexed draw.
        ID3D12CommandSignature* drawIndexed() const;

        /// Returns the command signature of an indirect mesh-task draw.
        ID3D12CommandSignature* mesh() const;

        /// Returns the command signature of an indirect dispatch.
        ID3D12CommandSignature* dispatch() const;

        /// Returns the serialized global root signature, kept for the device's life.
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
