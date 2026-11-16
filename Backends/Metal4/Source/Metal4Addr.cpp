//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Addr.cpp
/// @brief Implements address decoding and arithmetic for the Metal 4 backend, whose addresses are GPU virtual addresses.
//----------------------------------------------------------------------------------------------------------------------

#include <rojoRHI/RHI.h>

#include "Core/Addresses.h"
#include "Core/Buffers.h"
#include "Core/DeviceCore.h"
#include "Metal4AddressMap.h"
#include "Metal4Device.h"

#include <cstdint>

namespace rojoRHI {
    namespace core {
        //==============================================================================================================
        DecodedAddr decodeAddr(DeviceCore& core, Addr addr)
        {
            // The retire hooks' context is the device that owns the core. Only the address map's mutex is taken.
            const DeviceImpl& impl = *static_cast<const DeviceImpl*>(core.retireHooks().context);
            return impl.addresses.find(uint64_t{ addr.lo } | uint64_t{ addr.hi } << 32);
        }
    } // namespace core

    //==================================================================================================================
    Addr operator+(Addr base, uint64_t offset)
    {
        // A sum past the buffer is VU-rojo-buffer-0009 where the address is used; here only the offset is checked.
        core::checkAddrOffset(offset, metal4::maxAddrOffset());
        const uint64_t sum = (uint64_t{ base.lo } | uint64_t{ base.hi } << 32) + offset;
        return Addr{ static_cast<uint32_t>(sum), static_cast<uint32_t>(sum >> 32) };
    }
} // namespace rojoRHI
