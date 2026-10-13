//----------------------------------------------------------------------------------------------------------------------
/// @file TextureUsage.h
/// @brief Declares the creation-time usage flags of a texture and the usage of a view.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// What a backend must know about a texture at creation. Every texture is a copy source and destination without a flag.
    enum class TextureUsage : uint32_t
    {
        None = 0,               ///< No flag.
        Sampled = 1u << 0,      ///< Read through a sampled view.
        Storage = 1u << 1,      ///< Read and written through a storage view.
        RenderTarget = 1u << 2, ///< Color attachment.
        DepthStencil = 1u << 3, ///< Depth-stencil attachment.
        Sparse = 1u << 4,       ///< Reserved: no backing memory until tiles are mapped.
    };

    /// Returns the union of two usage sets.
    constexpr TextureUsage operator|(TextureUsage a, TextureUsage b)
    {
        return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /// Returns the flags both usage sets contain.
    constexpr TextureUsage operator&(TextureUsage a, TextureUsage b)
    {
        return static_cast<TextureUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
} // namespace rojoRHI
