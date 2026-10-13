//----------------------------------------------------------------------------------------------------------------------
/// @file Viewport.h
/// @brief Declares the viewport of a render pass.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

namespace rojoRHI {
    /// The single viewport, in framebuffer pixels with the origin at the top-left and `y` growing downwards; clip-space `+y`
    /// points up on every backend. `width` and `height` are greater than zero (VU-rojo-pipeline-0016) and `minDepth` and
    /// `maxDepth` lie in [0, 1] (VU-rojo-pipeline-0017).
    struct Viewport
    {
        float x = 0.0f;        ///< Left edge.
        float y = 0.0f;        ///< Top edge.
        float width = 0.0f;    ///< Width.
        float height = 0.0f;   ///< Height.
        float minDepth = 0.0f; ///< Depth mapped from clip-space zero.
        float maxDepth = 1.0f; ///< Depth mapped from clip-space one.
    };
} // namespace rojoRHI
