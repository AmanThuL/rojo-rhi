//----------------------------------------------------------------------------------------------------------------------
/// @file Access.h
/// @brief Declares the set of memory accesses a barrier names.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// A bit set of memory accesses; each bit is performed at the stages named after it. `AllGraphics` and `All` perform every
    /// access their stages perform.
    enum class Access : uint32_t
    {
        None = 0,                       ///< No memory access; the side orders execution only.
        IndirectArgs = 1u << 0,         ///< Reads of argument records and counts, at `Stage::IndirectArgs`.
        IndexRead = 1u << 1,            ///< Index fetch, at `Stage::IndexInput`.
        ShaderRead = 1u << 2,           ///< Shader reads through an `Addr`, a view or a root block, at any shader stage.
        ShaderWrite = 1u << 3,          ///< Shader writes and atomics through an `Addr` or a `Storage` view, at any shader stage.
        ColorAttachmentRead = 1u << 4,  ///< Attachment loads and blending, at `Stage::ColorAttachment`.
        ColorAttachmentWrite = 1u << 5, ///< Attachment stores and clears, at `Stage::ColorAttachment`.
        DepthStencilRead = 1u << 6,     ///< Depth and stencil tests, at `Stage::DepthStencil`.
        DepthStencilWrite = 1u << 7,    ///< Depth and stencil writes and clears, at `Stage::DepthStencil`.
        CopyRead = 1u << 8,             ///< Copy sources, at `Stage::Copy`.
        CopyWrite = 1u << 9,            ///< Copy, fill and clear destinations, at `Stage::Copy`.
        Resolve = 1u << 10,             ///< Writes to a resolve target, at `Stage::Resolve`.
    };

    /// Returns the union of two access sets.
    constexpr Access operator|(Access a, Access b)
    {
        return static_cast<Access>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /// Returns the accesses both sets contain.
    constexpr Access operator&(Access a, Access b)
    {
        return static_cast<Access>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    /// Returns the complement of an access set within the 32-bit mask.
    constexpr Access operator~(Access a)
    {
        return static_cast<Access>(~static_cast<uint32_t>(a));
    }
} // namespace rojoRHI
