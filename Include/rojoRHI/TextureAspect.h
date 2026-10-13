//----------------------------------------------------------------------------------------------------------------------
/// @file TextureAspect.h
/// @brief Declares the plane of a texture a view exposes.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The plane of a depth-stencil format a view selects, so the stencil of `D32FloatS8` can be sampled.
    enum class TextureAspect : uint8_t
    {
        Color,   ///< The color of a color format.
        Depth,   ///< The depth plane.
        Stencil, ///< The stencil plane.
    };
} // namespace rojoRHI
