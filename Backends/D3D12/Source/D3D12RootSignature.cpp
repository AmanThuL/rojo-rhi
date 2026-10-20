//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12RootSignature.cpp
/// @brief Stubs the D3D12 root and command signatures: creation makes none yet, so each accessor returns null.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12RootSignature.h"

#include <cstddef>
#include <span>

namespace rojoRHI::d3d12 {
    //==================================================================================================================
    Result<void> RootSignatures::create(DeviceImpl&)
    {
        return {};
    }

    //==================================================================================================================
    ID3D12RootSignature* RootSignatures::global() const
    {
        return m_global.get();
    }

    //==================================================================================================================
    ID3D12RootSignature* RootSignatures::kernel() const
    {
        return m_kernel.get();
    }

    //==================================================================================================================
    ID3D12CommandSignature* RootSignatures::draw() const
    {
        return m_draw.get();
    }

    //==================================================================================================================
    ID3D12CommandSignature* RootSignatures::drawIndexed() const
    {
        return m_drawIndexed.get();
    }

    //==================================================================================================================
    ID3D12CommandSignature* RootSignatures::mesh() const
    {
        return m_mesh.get();
    }

    //==================================================================================================================
    ID3D12CommandSignature* RootSignatures::dispatch() const
    {
        return m_dispatch.get();
    }

    //==================================================================================================================
    std::span<const std::byte> RootSignatures::serializedGlobal() const
    {
        return m_serializedGlobal;
    }
} // namespace rojoRHI::d3d12
