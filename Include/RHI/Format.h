//----------------------------------------------------------------------------------------------------------------------
/// @file Format.h
/// @brief Declares the RHI pixel and attachment format enumeration.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

namespace lmx::rhi {

/// Identifies the storage and transfer format of an RHI texture attachment.
/// RGBA16Float is the scene-linear color format: half precision preserves radiance above 1.0.
enum class Format {
    Unknown,         ///< No color or depth format.
    BGRA8Unorm,      ///< Eight-bit normalized BGRA color.
    RGBA8Unorm,      ///< Eight-bit normalized RGBA color.
    RGBA8Unorm_sRGB, ///< Eight-bit RGBA color with sRGB transfer on sampling.
    RGBA16Float,     ///< Half-precision linear RGBA color.
    RG16Float,       ///< Half-precision two-channel linear data.
    R8Unorm,         ///< Eight-bit normalized single channel.
    BC1Unorm,        ///< BC1-compressed normalized linear color.
    BC1Unorm_sRGB,   ///< BC1-compressed color with sRGB transfer on sampling.
    D32Float,        ///< 32-bit floating-point depth.
    R16Float         ///< Half-precision single-channel linear data.
};

} // namespace lmx::rhi
