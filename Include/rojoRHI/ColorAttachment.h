//----------------------------------------------------------------------------------------------------------------------
/// @file ColorAttachment.h
/// @brief Declares one color attachment of a render pass.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/ClearColor.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/LoadOp.h>
#include <rojoRHI/StoreOp.h>

#include <cstdint>

namespace rojoRHI {
    /// One color attachment: a single mip and array layer (a cube face counts as a layer) of `texture`. `clear` is used when
    /// `load` is `Clear`. A non-null `resolve` receives the average of the samples at the end of the pass, written to its mip 0
    /// and layer 0. `texture` was created with `TextureUsage::RenderTarget`, is not a 3D texture, and `mip` and `layer` lie
    /// within it (VU-rojo-cmd-0005); `resolve` obeys VU-rojo-cmd-0071 and its format reports `FormatCapabilities::resolve`
    /// (VU-rojo-format-0003); both are in `Layout::ColorAttachment` at `beginRenderPass` (VU-rojo-cmd-0007).
    struct ColorAttachment
    {
        /// The attachment texture.
        Texture texture;
        /// The mip rendered to.
        uint32_t mip = 0;
        /// The array layer rendered to.
        uint32_t layer = 0;
        /// What the attachment holds at the start of the pass.
        LoadOp load = LoadOp::Load;
        /// Whether the contents are kept at the end of the pass.
        StoreOp store = StoreOp::Store;
        /// The clear value.
        ClearColor clear;
        /// The single-sample texture that receives the resolved samples, or null.
        Texture resolve = {};
    };
} // namespace rojoRHI
