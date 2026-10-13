//----------------------------------------------------------------------------------------------------------------------
/// @file Addresses.h
/// @brief Declares the one function that resolves an address to the buffer it names, shared by every command that takes one.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Addr.h>
#include <rojoRHI/Handles.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Records.h"

#include <cstdint>

namespace rojoRHI::core {
    /// What a backend's encoding of a non-null `Addr` says: which buffer slot it names and how many bytes into the buffer.
    struct DecodedAddr
    {
        uint32_t slot;   ///< The slot index of the buffer in the buffer pool.
        uint64_t offset; ///< Bytes from the start of the buffer.
    };

    /// Decodes a non-null `addr` as the backend encodes it. Each backend defines it, as it does `coreOf`: Null keeps the slot
    /// index in `lo` and the byte offset in `hi`. An address carries no generation, so the decode ignores it: an address
    /// into a destroyed buffer whose slot was reused resolves to the new buffer.
    DecodedAddr decodeAddr(Addr addr);

    /// The buffer an address names.
    struct ResolvedAddr
    {
        Buffer buffer = {};                   ///< The buffer's handle; null for a null address.
        const BufferRecord* record = nullptr; ///< The buffer's record; null for a null address.
        uint64_t offset = 0;                  ///< Bytes from the start of the buffer.
    };

    /// Resolves `addr`, consumed by a command recorded on `list`, to its buffer and byte offset: the one site of
    /// VU-rojo-buffer-0009 (the address names a live buffer and its position lies inside it) and of VU-rojo-buffer-0020
    /// (frame data is used on a list of the frame that produced it). The null address resolves to a null buffer with no rule
    /// issued; the caller decides whether null is allowed. Every command that takes an `Addr` calls it in Debug builds only:
    /// it takes the buffer pool's lock, which Release recording never does (ADR 0019).
    ///
    /// VU-rojo-buffer-0020 compares the list's frame with the frame stamped on the page's record when a list claims it, so
    /// it does not see a stale address into a page a list of the later frame has claimed again, an address reached through
    /// root data or other buffer memory, or a shader's read; and an address into a reused slot names the new buffer.
    ResolvedAddr resolveAddr(DeviceCore& core, const CommandListCore& list, Addr addr);
} // namespace rojoRHI::core
