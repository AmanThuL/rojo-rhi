//----------------------------------------------------------------------------------------------------------------------
/// @file BufferUsage.h
/// @brief Declares the creation-time usage flags of a buffer.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// What a backend must know about a buffer at creation. Every `Gpu` and `Upload` buffer is shader-readable by address,
    /// and copy roles follow the memory class, so only these four flags exist.
    enum class BufferUsage : uint32_t
    {
        None = 0,               ///< No flag.
        Index = 1u << 0,        ///< Index buffer of an indexed draw.
        IndirectArgs = 1u << 1, ///< Arguments of an indirect draw or dispatch.
        ShaderWrite = 1u << 2,  ///< Written by shaders through an `Addr`; a `Memory::Gpu` buffer only.
        Sparse = 1u << 3,       ///< Reserved: no backing memory until tiles are mapped; a `Memory::Gpu` buffer only.
    };

    /// Returns the union of two usage sets.
    constexpr BufferUsage operator|(BufferUsage a, BufferUsage b)
    {
        return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /// Returns the flags both usage sets contain.
    constexpr BufferUsage operator&(BufferUsage a, BufferUsage b)
    {
        return static_cast<BufferUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
} // namespace rojoRHI
