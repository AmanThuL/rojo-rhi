//----------------------------------------------------------------------------------------------------------------------
/// @file TextureBarrier.h
/// @brief Declares the barrier that orders and transitions a range of one texture.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Access.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/Layout.h>
#include <rojoRHI/Stage.h>
#include <rojoRHI/SubresourceRange.h>

namespace rojoRHI {
    /// A memory dependency and layout transition for a range of one texture. `range` obeys VU-rojo-texture-0016, 0017 and 0041;
    /// `newLayout` is not `Undefined` (VU-rojo-sync-0014) and is allowed by the texture's usage (VU-rojo-sync-0015); the
    /// accesses are permitted by their layouts (VU-rojo-sync-0016); the sides obey VU-rojo-sync-0001 to 0003. `oldLayout` is
    /// checked by the validation tracker (VU-rojo-sync-0013).
    struct TextureBarrier
    {
        /// The texture whose subresources the barrier orders.
        Texture texture;
        /// The mips and layers covered.
        SubresourceRange range;
        /// Stages of earlier work the barrier waits for.
        Stage srcStage = Stage::None;
        /// Accesses of earlier work the barrier makes visible.
        Access srcAccess = Access::None;
        /// Stages of later work that wait.
        Stage dstStage = Stage::None;
        /// Accesses of later work that see the source writes.
        Access dstAccess = Access::None;
        /// The layout every covered subresource is in; `Undefined` discards the contents.
        Layout oldLayout = Layout::Undefined;
        /// The layout every covered subresource is in after the barrier.
        Layout newLayout = Layout::Undefined;
    };
} // namespace rojoRHI
