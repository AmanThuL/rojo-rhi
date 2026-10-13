//----------------------------------------------------------------------------------------------------------------------
/// @file UpscalerKind.h
/// @brief Declares the upscaler implementations a device can offer.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// Selects an upscaler implementation. Requesting a kind that is not compiled in, or that the device or driver refuses,
    /// is an error at creation, never a silent substitution.
    enum class UpscalerKind : uint8_t
    {
        Auto,    ///< The backend's preferred available kind.
        MetalFX, ///< Apple MetalFX temporal scaler.
        DLSS,    ///< NVIDIA DLSS super resolution.
        FSR,     ///< AMD FidelityFX Super Resolution, temporal.
        XeSS,    ///< Intel XeSS super resolution.
    };
} // namespace rojoRHI
