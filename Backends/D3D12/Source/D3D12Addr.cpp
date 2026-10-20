//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Addr.cpp
/// @brief Stubs the D3D12 address lowering: the backend creates no buffer yet, so no address exists to decode or offset.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Addr.h"

#include <rojoRHI/RHI.h>

#include "Core/Addresses.h"
#include "D3D12Common.h"

#include <cstdint>

namespace rojoRHI {
    namespace d3d12 {
        //==============================================================================================================
        void HeapDirectory::create(uint32_t)
        {
        }

        //==============================================================================================================
        void HeapDirectory::publish(uint32_t, const BufferPayload*)
        {
            notImplemented("HeapDirectory::publish");
        }

        //==============================================================================================================
        void HeapDirectory::clear(uint32_t)
        {
            notImplemented("HeapDirectory::clear");
        }

        //==============================================================================================================
        const BufferPayload* HeapDirectory::find(uint32_t) const
        {
            notImplemented("HeapDirectory::find");
        }
    } // namespace d3d12

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
