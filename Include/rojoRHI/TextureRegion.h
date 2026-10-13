//----------------------------------------------------------------------------------------------------------------------
/// @file TextureRegion.h
/// @brief Declares a box of texels in one mip of a range of array layers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Geometry.h>

#include <cstdint>

namespace rojoRHI {
    /// A box of texels in one mip of a range of array layers. `mip` is below the texture's mip count, `layers` is at least one
    /// and `layer + layers` is at most its layer count (VU-rojo-cmd-0038); `extent` is non-zero and `offset + extent` lies
    /// within the mip (VU-rojo-cmd-0039); for a block-compressed format `offset` is a multiple of the block size and `extent`
    /// is a multiple of it or reaches the mip's edge (VU-rojo-cmd-0040); the format has no stencil component
    /// (VU-rojo-cmd-0041).
    struct TextureRegion
    {
        /// Mip level.
        uint32_t mip = 0;
        /// First array layer.
        uint32_t layer = 0;
        /// Number of array layers.
        uint32_t layers = 1;
        /// First texel.
        Offset3D offset;
        /// Size in texels.
        Extent3D extent;
    };
} // namespace rojoRHI
