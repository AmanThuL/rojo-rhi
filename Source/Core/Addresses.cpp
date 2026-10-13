//----------------------------------------------------------------------------------------------------------------------
/// @file Addresses.cpp
/// @brief Implements the resolution of an address to its buffer.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Addresses.h"

#include "Base/Assert.h"

namespace rojoRHI::core {
    //==================================================================================================================
    ResolvedAddr resolveAddr(DeviceCore& core, [[maybe_unused]] const CommandListCore& list, Addr addr)
    {
        if (addr.lo == 0 && addr.hi == 0)
        {
            return ResolvedAddr{};
        }
        const DecodedAddr decoded = decodeAddr(addr);
        const auto live = core.pool<Buffer>().liveAt(decoded.slot);
        ROJORHI_VU(
            live && decoded.offset < live->record->size,
            "VU-rojo-buffer-0009",
            "the address {{{:#x}, {:#x}}} does not name a byte inside a live buffer",
            addr.lo,
            addr.hi
        );
        if (!live)
        {
            return ResolvedAddr{}; // Release only: the address is not a buffer's, and nothing here may read it.
        }
#ifndef NDEBUG
        // The claiming frame changes under the pool's lock when a list claims the page, so it is read under that lock too.
        uint64_t arenaFrame = 0;
        core.pool<Buffer>().update(live->handle, [&](const BufferRecord& record) { arenaFrame = record.arenaFrame; });
        ROJORHI_VU(
            arenaFrame == 0 || arenaFrame == list.frame,
            "VU-rojo-buffer-0020",
            "the address {{{:#x}, {:#x}}} is frame data of another frame, used on the list \"{}\" of frame {}",
            addr.lo,
            addr.hi,
            list.label,
            list.frame
        );
#endif
        return ResolvedAddr{ live->handle, live->record, decoded.offset };
    }
} // namespace rojoRHI::core
