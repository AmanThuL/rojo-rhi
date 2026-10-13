//----------------------------------------------------------------------------------------------------------------------
/// @file Copies.h
/// @brief Declares the checks of the copy, fill and clear rules of chapter 07.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/BufferTextureLayout.h>
#include <rojoRHI/ClearValue.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/SubresourceRange.h>
#include <rojoRHI/TextureRegion.h>

#include "Core/CommandLists.h"

#include <cstdint>

namespace rojoRHI::core {
    /// Checks a buffer copy (VU-rojo-cmd-0063, 0042 to 0044, VU-rojo-buffer-0001, 0002, VU-rojo-core-0001 to 0003).
    void copyBuffer(CommandListCore& list, Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset, uint64_t size);

    /// Checks a copy from a buffer into a texture (VU-rojo-cmd-0063, 0046, 0064, 0066, 0034 to 0041, VU-rojo-buffer-0002,
    /// VU-rojo-core-0001 to 0003).
    void copyBufferToTexture(CommandListCore& list, Buffer src, const BufferTextureLayout& layout, Texture dst, const TextureRegion& region);

    /// Checks a copy from a texture into a buffer (VU-rojo-cmd-0063, 0049, 0065, 0068, 0034 to 0041, VU-rojo-buffer-0001,
    /// VU-rojo-core-0001 to 0003).
    void copyTextureToBuffer(CommandListCore& list, Texture src, const TextureRegion& region, Buffer dst, const BufferTextureLayout& layout);

    /// Checks a copy between textures (VU-rojo-cmd-0063, 0052, 0053, 0055, 0038 to 0041, VU-rojo-core-0001 to 0003).
    void copyTexture(CommandListCore& list, Texture src, const TextureRegion& srcRegion, Texture dst, const TextureRegion& dstRegion);

    /// Checks a buffer fill (VU-rojo-cmd-0063, 0056 to 0058, VU-rojo-core-0001 to 0003).
    void fillBuffer(CommandListCore& list, Buffer dst, uint64_t offset, uint64_t size);

    /// Checks a texture clear (VU-rojo-cmd-0063, 0059 to 0061, VU-rojo-texture-0016, 0017, 0041, VU-rojo-core-0001 to 0003).
    void clearTexture(CommandListCore& list, Texture texture, const SubresourceRange& range, const ClearValue& value);
} // namespace rojoRHI::core
