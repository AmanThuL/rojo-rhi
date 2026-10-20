//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Stager.cpp
/// @brief Stubs the D3D12 stager: creation makes no upload queue yet, and an upload aborts.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Stager.h"

#include "D3D12Common.h"

#include <cstddef>
#include <span>

namespace rojoRHI::d3d12 {
    //==================================================================================================================
    Result<void> Stager::create(DeviceImpl&)
    {
        return {};
    }

    //==================================================================================================================
    Result<void> Stager::uploadBuffer(DeviceImpl&, ID3D12Resource*, std::span<const std::byte>)
    {
        notImplemented("Stager::uploadBuffer");
    }

    //==================================================================================================================
    Result<void> Stager::uploadTexture(DeviceImpl&, ID3D12Resource*, std::span<const SubresourceData>)
    {
        notImplemented("Stager::uploadTexture");
    }

    //==================================================================================================================
    void Stager::idle()
    {
    }
} // namespace rojoRHI::d3d12
