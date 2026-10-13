//----------------------------------------------------------------------------------------------------------------------
/// @file UpscalerFlags.h
/// @brief Declares the creation-time behaviour flags of an upscaler.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// A bit set fixed at creation by `UpscalerDesc::flags`.
    enum class UpscalerFlags : uint32_t
    {
        None = 0,                ///< No flag.
        HDR = 1u << 0,           ///< Color input is high dynamic range scene-linear, not display-referred.
        ReversedDepth = 1u << 1, ///< Larger depth values are nearer the camera.
        AutoExposure = 1u << 2,  ///< The implementation computes exposure itself; `UpscaleParams::exposure` is ignored.
    };

    /// Returns the union of two flag sets.
    constexpr UpscalerFlags operator|(UpscalerFlags a, UpscalerFlags b)
    {
        return static_cast<UpscalerFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /// Returns the flags both sets contain.
    constexpr UpscalerFlags operator&(UpscalerFlags a, UpscalerFlags b)
    {
        return static_cast<UpscalerFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
} // namespace rojoRHI
