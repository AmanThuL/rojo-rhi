//----------------------------------------------------------------------------------------------------------------------
/// @file DepthAttachment.h
/// @brief Declares the depth and stencil attachment of a render pass.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Handles.h>
#include <rojoRHI/LoadOp.h>
#include <rojoRHI/StoreOp.h>

#include <cstdint>

namespace rojoRHI {
    /// The depth and stencil attachment of a render pass; a null `texture` means none. `load`, `store` and `clearDepth` govern
    /// depth; the stencil fields govern stencil and are ignored for formats without one. A non-null `texture` was created with
    /// `TextureUsage::DepthStencil` in a depth or depth-stencil format, and `mip` and `layer` lie within it (VU-rojo-cmd-0008);
    /// `clearDepth` lies in [0, 1] (VU-rojo-cmd-0009); the subresource is in `Layout::DepthStencilWrite` at `beginRenderPass`
    /// (VU-rojo-cmd-0010).
    struct DepthAttachment
    {
        /// The attachment texture, or null for none.
        Texture texture;
        /// The mip rendered to.
        uint32_t mip = 0;
        /// The array layer rendered to.
        uint32_t layer = 0;
        /// What depth holds at the start of the pass.
        LoadOp load = LoadOp::Load;
        /// Whether depth is kept at the end of the pass.
        StoreOp store = StoreOp::Store;
        /// The depth clear value.
        float clearDepth = 0.0f;
        /// What stencil holds at the start of the pass.
        LoadOp stencilLoad = LoadOp::DontCare;
        /// Whether stencil is kept at the end of the pass.
        StoreOp stencilStore = StoreOp::DontCare;
        /// The stencil clear value.
        uint8_t clearStencil = 0;
    };
} // namespace rojoRHI
