//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Textures.cpp
/// @brief Stubs the D3D12 texture entries: the backend creates no texture yet.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Textures.h"

#include <rojoRHI/RHI.h>

#include "Core/Records.h"
#include "D3D12Common.h"

#include <cstdint>
#include <span>

namespace rojoRHI {
    namespace d3d12 {
        //==============================================================================================================
        D3D12_CPU_DESCRIPTOR_HANDLE targetView(const TexturePayload&, uint32_t, uint32_t)
        {
            notImplemented("d3d12::targetView");
        }

        //==============================================================================================================
        void retireTexture(void*, core::TextureRecord&)
        {
            notImplemented("d3d12::retireTexture");
        }
    } // namespace d3d12

    //==================================================================================================================
    Result<Texture> Device::createTexture(const TextureDesc&, std::span<const TextureData>, Placement)
    {
        return d3d12::unsupported("Device::createTexture");
    }

    //==================================================================================================================
    PlacementRequirements Device::placementRequirements(const TextureDesc&)
    {
        d3d12::notImplemented("Device::placementRequirements(TextureDesc)");
    }

    //==================================================================================================================
    void Device::destroy(Texture)
    {
        d3d12::notImplemented("Device::destroy(Texture)");
    }
} // namespace rojoRHI
