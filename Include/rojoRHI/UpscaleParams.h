//----------------------------------------------------------------------------------------------------------------------
/// @file UpscaleParams.h
/// @brief Declares the per-frame inputs of one upscale.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Geometry.h>
#include <rojoRHI/Handles.h>

#include <string_view>

namespace rojoRHI {
    /// The arguments of `CommandList::upscale`. `preExposure` is greater than zero (VU-rojo-upscale-0004) and `sharpness` is
    /// from 0 to 1 (VU-rojo-upscale-0005); the textures obey VU-rojo-upscale-0009 to 0015.
    struct UpscaleParams
    {
        /// Jittered color input; scene-linear and pre-exposed by `preExposure` with `UpscalerFlags::HDR`.
        Texture color;
        /// This frame's depth.
        Texture depth;
        /// Per-pixel motion, scaled by `motionScaleX` and `motionScaleY`; it points from the current position to the previous
        /// one, in input pixels.
        Texture motion;
        /// Reconstructed result at the descriptor's `output` extent.
        Texture output;
        /// Optional 1x1 exposure multiplier for the color input; null means 1.0; ignored with `UpscalerFlags::AutoExposure`.
        Texture exposure = {};
        /// Optional per-pixel weight; 1.0 asks for no history accumulation; null means none.
        Texture reactive = {};
        /// Active origin-anchored rectangle of the inputs; zero in both axes means the whole `input` extent.
        Extent2D inputContent = {};
        /// Sub-pixel raster jitter of this frame in x, in input texels, positive down.
        float jitterX = 0.0f;
        /// Sub-pixel raster jitter of this frame in y, in input texels, positive down.
        float jitterY = 0.0f;
        /// Converts stored motion to input pixels along x.
        float motionScaleX = 1.0f;
        /// Converts stored motion to input pixels along y.
        float motionScaleY = 1.0f;
        /// Positive multiplier already applied to the color input.
        float preExposure = 1.0f;
        /// Sharpening strength from 0 to 1; implementations without a sharpening pass ignore it.
        float sharpness = 0.0f;
        /// Discards the history before this frame, as after a camera cut.
        bool reset = false;
        /// Debug-group label of the operation in captures and scope timings, copied while recording; empty selects
        /// `rojorhi.upscale.unnamed`.
        std::string_view label;
    };
} // namespace rojoRHI
