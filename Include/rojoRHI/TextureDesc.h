//----------------------------------------------------------------------------------------------------------------------
/// @file TextureDesc.h
/// @brief Declares the description of a texture.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Format.h>
#include <rojoRHI/Geometry.h>
#include <rojoRHI/TextureType.h>
#include <rojoRHI/TextureUsage.h>

#include <cstdint>
#include <string_view>

namespace rojoRHI {
    /// Describes a texture for `Device::createTexture` and `Device::placementRequirements`. The format is not `Undefined`
    /// (VU-rojo-texture-0001); every extent component is at least 1 and within the limit for the type (VU-rojo-texture-0002),
    /// and the type fixes the height, depth, width and layers (VU-rojo-texture-0003 to 0005), which are at most
    /// `Limits::maxTextureLayers` (VU-rojo-texture-0039). `mips` is within the extent
    /// (VU-rojo-texture-0006). `samples` is supported for the format and constrains the type, mips and usage
    /// (VU-rojo-texture-0007, 0008). The usage is not empty, is supported for the format and combines only as
    /// VU-rojo-texture-0009 to 0012 allow; a block-compressed format has a level-0 width and height that are multiples of 4
    /// (VU-rojo-format-0004).
    struct TextureDesc
    {
        /// Dimensionality; `Tex1D`, `Tex2D`, `Tex3D`, `Cube` and the array forms.
        TextureType type = TextureType::Tex2D;
        /// Storage format.
        Format format = Format::Undefined;
        /// Texels of mip 0.
        Extent3D extent;
        /// Mip levels.
        uint32_t mips = 1;
        /// Array layers; for cube types, faces.
        uint32_t layers = 1;
        /// Samples per texel.
        uint32_t samples = 1;
        /// Sampled, Storage, RenderTarget, DepthStencil and Sparse.
        TextureUsage usage = TextureUsage::None;
        /// Debug label; empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };
} // namespace rojoRHI
