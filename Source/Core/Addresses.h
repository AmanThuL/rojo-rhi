//----------------------------------------------------------------------------------------------------------------------
/// @file Addresses.h
/// @brief Declares the one function that resolves an address to the buffer it names, shared by every command that takes one.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Records.h"

#include <cstdint>

namespace rojoRHI::core {
    struct DecodedAddr
    {
        uint32_t slot;
        uint64_t offset;
    };

    // Each backend defines it, as it does `coreOf`. An address outside every live buffer decodes to slot 0, which names no
    // buffer. An address carries no generation: one into a destroyed buffer whose slot was reused resolves to the new buffer.
    DecodedAddr decodeAddr(DeviceCore& core, Addr addr);

    struct ResolvedAddr
    {
        Buffer buffer = {};
        const BufferRecord* record = nullptr;
        uint64_t offset = 0;
    };

    // The one site of VU-rojo-buffer-0009 and VU-rojo-buffer-0020; a null address resolves to a null buffer unchecked. Callers
    // call it in Debug only: it takes the buffer pool's lock, which Release recording never does (ADR 0020). VU-rojo-buffer-0020
    // misses an address into a page a later frame claimed again, and any address reached through root data, memory or a shader.
    ResolvedAddr resolveAddr(DeviceCore& core, const CommandListCore& list, Addr addr);
} // namespace rojoRHI::core
