//----------------------------------------------------------------------------------------------------------------------
/// @file ColorTarget.h
/// @brief Declares one color output of a graphics pipeline.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/BlendState.h>
#include <rojoRHI/Format.h>

#include <cstdint>

namespace rojoRHI {
    /// The channels a color target writes; a bit set.
    enum class ColorWriteMask : uint8_t
    {
        None = 0, ///< No channel.
        R = 1,    ///< Red.
        G = 2,    ///< Green.
        B = 4,    ///< Blue.
        A = 8,    ///< Alpha.
        All = 15, ///< Every channel.
    };

    /// Returns the union of two channel sets.
    constexpr ColorWriteMask operator|(ColorWriteMask a, ColorWriteMask b)
    {
        return static_cast<ColorWriteMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }

    /// Returns the channels both sets contain.
    constexpr ColorWriteMask operator&(ColorWriteMask a, ColorWriteMask b)
    {
        return static_cast<ColorWriteMask>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
    }

    /// One color output of a graphics pipeline: the attachment format, its blend state and the channels written. Entry `i` of
    /// `GraphicsPipelineDesc::colors` matches color attachment `i` of the render pass. `format` is not `Undefined`
    /// (VU-rojo-format-0002) and reports `FormatCapabilities::colorAttachment` (VU-rojo-format-0003).
    struct ColorTarget
    {
        /// The attachment format.
        Format format = Format::Undefined;
        /// Blending of this target.
        BlendState blend;
        /// The channels written.
        ColorWriteMask writeMask = ColorWriteMask::All;
    };
} // namespace rojoRHI
