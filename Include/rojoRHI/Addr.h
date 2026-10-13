//----------------------------------------------------------------------------------------------------------------------
/// @file Addr.h
/// @brief Declares the shader-visible address of buffer memory and its offset arithmetic.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// An 8-byte value naming buffer memory, with a backend-defined encoding; the value-initialized `Addr{}` is the
    /// null address on every backend.
    struct alignas(8) Addr
    {
        /// First word of the encoding.
        uint32_t lo;
        /// Second word of the encoding.
        uint32_t hi;
    };

    /// Returns the address `offset` bytes further into the buffer `base` names. The result lies less than
    /// `Limits::maxAddrOffset` bytes from the buffer's start (VU-rojo-buffer-0011).
    Addr operator+(Addr base, uint64_t offset);
} // namespace rojoRHI
