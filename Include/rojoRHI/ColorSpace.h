//----------------------------------------------------------------------------------------------------------------------
/// @file ColorSpace.h
/// @brief Declares the color spaces a swapchain can present in.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// How the display interprets the values written to a swapchain image. Each color space admits one format: `SRGB` takes
    /// `Format::BGRA8Unorm`, `ExtendedLinear` takes `Format::RGBA16Float` and `HDR10` takes `Format::RGB10A2Unorm`
    /// (VU-rojo-present-0003).
    enum class ColorSpace : uint8_t
    {
        SRGB,           ///< Standard dynamic range, sRGB primaries and transfer; the shader writes sRGB-encoded values.
        ExtendedLinear, ///< Extended dynamic range: linear, sRGB primaries, values above 1.0 brighter than SDR white.
        HDR10,          ///< BT.2020 primaries, SMPTE ST 2084 (PQ) transfer.
    };
} // namespace rojoRHI
