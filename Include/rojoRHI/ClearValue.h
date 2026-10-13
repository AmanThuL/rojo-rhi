//----------------------------------------------------------------------------------------------------------------------
/// @file ClearValue.h
/// @brief Declares the value `CommandList::clearTexture` writes.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/ClearColor.h>

#include <cstdint>

namespace rojoRHI {
    /// The value `CommandList::clearTexture` writes: `color` for color formats, `depth` and `stencil` for depth and
    /// depth-stencil formats. `depth` lies in [0, 1] (VU-rojo-cmd-0059).
    struct ClearValue
    {
        /// Written to a color format.
        ClearColor color;
        /// Written to a depth plane.
        float depth = 0.0f;
        /// Written to a stencil plane.
        uint8_t stencil = 0;
    };
} // namespace rojoRHI
