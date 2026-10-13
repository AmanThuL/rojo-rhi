//----------------------------------------------------------------------------------------------------------------------
/// @file BufferTextureLayout.h
/// @brief Declares how texel data is laid out in a buffer for a buffer-texture copy.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// How texel data is laid out in a buffer for `CommandList::copyBufferToTexture` and `CommandList::copyTextureToBuffer`.
    /// Depth slices follow each other, then layers, each `rowPitch * rowsPerImage` bytes apart. `offset` is a multiple of 512
    /// (VU-rojo-cmd-0034); `rowPitch` is a multiple of 256 and at least one row of the region's texel blocks
    /// (VU-rojo-cmd-0035); `rowsPerImage` is zero or at least the region's height in texel blocks (VU-rojo-cmd-0036); when the
    /// region spans more than one image, the distance between images is a multiple of 512 (VU-rojo-cmd-0037).
    struct BufferTextureLayout
    {
        /// Bytes from the buffer start to the first texel block.
        uint64_t offset = 0;
        /// Bytes between rows of texel blocks.
        uint32_t rowPitch = 0;
        /// Rows of blocks between depth slices and layers; 0 is the region height.
        uint32_t rowsPerImage = 0;
    };
} // namespace rojoRHI
