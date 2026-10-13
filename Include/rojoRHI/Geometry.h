//----------------------------------------------------------------------------------------------------------------------
/// @file Geometry.h
/// @brief Declares the shared extent, offset and rectangle value types.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// A width and height in texels or pixels, as the using entry states; the zero default lets an entry give `{}` a
    /// meaning of its own.
    struct Extent2D
    {
        uint32_t width = 0;  ///< Width.
        uint32_t height = 0; ///< Height.
    };

    /// A width, height and depth in texels; each member defaults to one, so a two-dimensional extent names only width
    /// and height.
    struct Extent3D
    {
        uint32_t width = 1;  ///< Width.
        uint32_t height = 1; ///< Height.
        uint32_t depth = 1;  ///< Depth.
    };

    /// A texel position inside one mip level; offsets are never negative.
    struct Offset3D
    {
        uint32_t x = 0; ///< Horizontal position.
        uint32_t y = 0; ///< Vertical position.
        uint32_t z = 0; ///< Depth position.
    };

    /// A rectangle in framebuffer pixels with its origin at the top-left.
    struct Rect2D
    {
        uint32_t x = 0;      ///< Left edge.
        uint32_t y = 0;      ///< Top edge.
        uint32_t width = 0;  ///< Width.
        uint32_t height = 0; ///< Height.
    };
} // namespace rojoRHI
