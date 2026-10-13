//----------------------------------------------------------------------------------------------------------------------
/// @file TextureViewDesc.h
/// @brief Declares the description of a texture view.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Format.h>
#include <rojoRHI/SubresourceRange.h>
#include <rojoRHI/TextureAspect.h>
#include <rojoRHI/TextureType.h>
#include <rojoRHI/TextureUsage.h>

namespace rojoRHI {
    /// Describes how a shader sees a texture through a view index, for `Device::createView`. `usage` is exactly `Sampled` or
    /// `Storage` and the texture has it (VU-rojo-texture-0022); `type` is compatible with the texture's (VU-rojo-texture-0023)
    /// and fixes the layers covered (VU-rojo-texture-0024); `format` is `Undefined`, the texture's format or its sRGB or
    /// linear counterpart (VU-rojo-texture-0025) and supports `usage` (VU-rojo-format-0003); a `Storage` view covers one mip
    /// of a single-sample texture (VU-rojo-texture-0026); `aspect` fits the texture's format (VU-rojo-texture-0027).
    struct TextureViewDesc
    {
        /// Dimensionality the shader fetches.
        TextureType type = TextureType::Tex2D;
        /// `Undefined` selects the texture's own format.
        Format format = Format::Undefined;
        /// Mips and layers exposed.
        SubresourceRange range;
        /// Exactly `Sampled` or `Storage`.
        TextureUsage usage = TextureUsage::Sampled;
        /// Plane of a depth-stencil format.
        TextureAspect aspect = TextureAspect::Color;
    };
} // namespace rojoRHI
