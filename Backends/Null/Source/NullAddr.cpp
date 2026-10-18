//----------------------------------------------------------------------------------------------------------------------
/// @file NullAddr.cpp
/// @brief Implements address arithmetic for the Null backend's address encoding.
//----------------------------------------------------------------------------------------------------------------------

#include <rojoRHI/RHI.h>

#include "Core/Addresses.h"
#include "Core/Buffers.h"
#include "NullDevice.h"

namespace rojoRHI {
    namespace core {
        //==============================================================================================================
        DecodedAddr decodeAddr(Addr addr)
        {
            return DecodedAddr{ addr.lo, addr.hi };
        }
    } // namespace core

    //==================================================================================================================
    Addr operator+(Addr base, uint64_t offset)
    {
        // The Null encoding keeps the byte offset in `hi`; the offset is checked alone first so the sum cannot wrap.
        core::checkAddrOffset(offset, kNullMaxAddrOffset);
        const uint64_t sum = static_cast<uint64_t>(base.hi) + offset;
        core::checkAddrOffset(sum, kNullMaxAddrOffset);
        return Addr{ base.lo, static_cast<uint32_t>(sum) };
    }
} // namespace rojoRHI
