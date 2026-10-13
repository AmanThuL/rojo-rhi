//----------------------------------------------------------------------------------------------------------------------
/// @file TextureType.h
/// @brief Declares the dimensionality of a texture or a texture view.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The dimensionality of a texture or of the view a shader fetches. For `Cube` and `CubeArray`, array layers count faces,
    /// six per cube, in the order +X, -X, +Y, -Y, +Z, -Z. Multisampling is a property of `TextureDesc::samples`, not a type.
    enum class TextureType : uint8_t
    {
        Tex1D,      ///< One-dimensional.
        Tex1DArray, ///< Array of one-dimensional layers.
        Tex2D,      ///< Two-dimensional.
        Tex2DArray, ///< Array of two-dimensional layers.
        Tex3D,      ///< Three-dimensional.
        Cube,       ///< Six square faces.
        CubeArray,  ///< A multiple of six square faces.
    };
} // namespace rojoRHI
