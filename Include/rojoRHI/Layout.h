//----------------------------------------------------------------------------------------------------------------------
/// @file Layout.h
/// @brief Declares the layout of a texture subresource.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The layout of a texture subresource: the D3D12 enhanced-barrier core, mapped onto Vulkan image layouts and ignored by
    /// Metal 4. Buffers have no layout. The caller states both the layout a subresource is in and the one it moves to; a new
    /// texture starts `Undefined`, or `ShaderRead` when created with initial data.
    enum class Layout : uint8_t
    {
        Undefined,         ///< Contents are undefined; valid only before a barrier, which discards the contents.
        General,           ///< Shader reads, copy reads and copy writes; the layout of a cross-queue handoff.
        ShaderRead,        ///< Shader reads through `Sampled` views.
        ShaderWrite,       ///< Shader reads and writes through `Storage` views.
        ColorAttachment,   ///< Color attachment use, and the target of a render-pass resolve.
        DepthStencilWrite, ///< Depth-stencil attachment use with or without writes.
        DepthStencilRead,  ///< Shader reads of a depth-stencil texture through `Sampled` views.
        CopySrc,           ///< Copy source.
        CopyDst,           ///< Copy, fill and clear destination.
        Present,           ///< Handed to presentation; permits no other use.
    };
} // namespace rojoRHI
