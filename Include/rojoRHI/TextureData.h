//----------------------------------------------------------------------------------------------------------------------
/// @file TextureData.h
/// @brief Declares the initial contents of one texture subresource.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace rojoRHI {
    /// The initial contents of one subresource for `Device::createTexture`. Rows of a block-compressed format are rows of
    /// blocks, counted with rounding up. When `bytes` is not empty, `bytesPerRow` is at least the packed row size and a
    /// multiple of the texel or block size (VU-rojo-texture-0013), `bytesPerImage` is at least `bytesPerRow` times the rows of
    /// one slice for a `Tex3D` mip and 0 otherwise (VU-rojo-texture-0014), and `bytes` holds the bytes the strides imply for
    /// the subresource (VU-rojo-texture-0015).
    struct TextureData
    {
        /// The texels; empty leaves the subresource undefined.
        std::span<const std::byte> bytes;
        /// Distance in bytes between rows of texels or compressed blocks.
        uint64_t bytesPerRow = 0;
        /// Distance in bytes between depth slices of a `Tex3D` mip; 0 otherwise.
        uint64_t bytesPerImage = 0;
    };
} // namespace rojoRHI
