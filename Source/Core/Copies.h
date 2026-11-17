//----------------------------------------------------------------------------------------------------------------------
/// @file Copies.h
/// @brief Declares the checks of the copy, fill and clear rules of chapter 07.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"

#include <cstdint>

namespace rojoRHI::core {
    void copyBuffer(CommandListCore& list, Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset, uint64_t size);

    void copyBufferToTexture(CommandListCore& list, Buffer src, const BufferTextureLayout& layout, Texture dst, const TextureRegion& region);

    void copyTextureToBuffer(CommandListCore& list, Texture src, const TextureRegion& region, Buffer dst, const BufferTextureLayout& layout);

    void copyTexture(CommandListCore& list, Texture src, const TextureRegion& srcRegion, Texture dst, const TextureRegion& dstRegion);

    void fillBuffer(CommandListCore& list, Buffer dst, uint64_t offset, uint64_t size);

    void clearTexture(CommandListCore& list, Texture texture, const SubresourceRange& range, const ClearValue& value);
} // namespace rojoRHI::core
