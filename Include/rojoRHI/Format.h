//----------------------------------------------------------------------------------------------------------------------
/// @file Format.h
/// @brief Declares the texture formats the interface names.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// Names the storage layout and transfer function of a texture, a texture view, an attachment or a pipeline target. New
    /// formats are appended in later API versions; existing values never change.
    enum class Format : uint32_t
    {
        Undefined,       ///< No format.
        BGRA8Unorm,      ///< 8-bit normalized BGRA color.
        RGBA8Unorm,      ///< 8-bit normalized RGBA color.
        RGBA8Unorm_sRGB, ///< 8-bit RGBA color, sRGB transfer.
        RGBA16Float,     ///< Half-precision RGBA, the scene-linear color format.
        RG16Float,       ///< Half-precision two-channel data.
        R8Unorm,         ///< 8-bit normalized single channel.
        BC1Unorm,        ///< BC1 block-compressed linear color.
        BC1Unorm_sRGB,   ///< BC1 block-compressed color, sRGB transfer.
        D32Float,        ///< 32-bit floating-point depth.
        R16Float,        ///< Half-precision single channel.
        R32Float,        ///< Full-precision single channel.
        D32FloatS8,      ///< 32-bit floating-point depth with 8-bit stencil.
        RGB10A2Unorm,    ///< 10-bit normalized RGB with 2-bit alpha, the HDR10 presentation format.
    };
} // namespace rojoRHI
