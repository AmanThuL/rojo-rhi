//----------------------------------------------------------------------------------------------------------------------
/// @file SamplerAddress.h
/// @brief Declares the address mode of a sampler.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// How a sampler treats coordinates outside the texture.
    enum class SamplerAddress : uint8_t
    {
        Repeat,       ///< The coordinate wraps.
        MirrorRepeat, ///< The coordinate wraps, mirrored on every other repeat.
        ClampToEdge,  ///< The coordinate clamps to the edge texel.
    };
} // namespace rojoRHI
