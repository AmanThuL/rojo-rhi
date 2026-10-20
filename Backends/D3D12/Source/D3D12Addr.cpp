//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Addr.cpp
/// @brief Defines address arithmetic for the D3D12 backend, which creates no device, so no address exists to decode or offset.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include <rojoRHI/RHI.h>

#include "Core/Addresses.h"

#include <cstdint>

namespace rojoRHI {
    namespace core {
        //==============================================================================================================
        DecodedAddr decodeAddr(DeviceCore&, Addr)
        {
            d3d12::notImplemented("core::decodeAddr");
        }
    } // namespace core

    //==================================================================================================================
    Addr operator+(Addr, uint64_t)
    {
        d3d12::notImplemented("operator+(Addr, uint64_t)");
    }
} // namespace rojoRHI
