//----------------------------------------------------------------------------------------------------------------------
/// @file UpscalerDesc.h
/// @brief Declares the description of an upscaler.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Format.h>
#include <rojoRHI/Geometry.h>
#include <rojoRHI/UpscalerFlags.h>
#include <rojoRHI/UpscalerKind.h>

#include <string_view>

namespace rojoRHI {
    /// Describes an upscaler for `Device::createUpscaler`. `input` is greater than zero in both axes (VU-rojo-upscale-0001) and
    /// `output` is at least `input` in both (VU-rojo-upscale-0002); `color` and `outputFormat` are color formats, `depth` is a
    /// depth format and `motion` is a two-channel floating-point format (VU-rojo-upscale-0003). The active content inside
    /// `input` may shrink per frame through `UpscaleParams::inputContent`.
    struct UpscalerDesc
    {
        /// Implementation to create.
        UpscalerKind kind = UpscalerKind::Auto;
        /// Allocated extent of the color, depth, motion and reactive inputs, in texels.
        Extent2D input;
        /// Extent of the reconstructed output, in texels.
        Extent2D output;
        /// Format of the color input.
        Format color = Format::RGBA16Float;
        /// Format of the depth input.
        Format depth = Format::D32Float;
        /// Format of the two-channel motion input.
        Format motion = Format::RG16Float;
        /// Format of the reconstructed output.
        Format outputFormat = Format::RGBA16Float;
        /// Creation-time behaviour.
        UpscalerFlags flags = UpscalerFlags::None;
        /// Diagnostic label, copied at creation; empty selects `rojorhi.upscaler.unnamed`.
        std::string_view label;
    };
} // namespace rojoRHI
