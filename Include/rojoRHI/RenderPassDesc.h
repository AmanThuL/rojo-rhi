//----------------------------------------------------------------------------------------------------------------------
/// @file RenderPassDesc.h
/// @brief Declares the description of a render pass.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/ColorAttachment.h>
#include <rojoRHI/DepthAttachment.h>
#include <rojoRHI/Geometry.h>

#include <span>
#include <string_view>

namespace rojoRHI {
    /// The attachments of one render pass, the area it renders to and the label of the scope the pass forms; the span and label
    /// are copied. Load and store operations may affect the whole attachment subresource, not only `area`. `colors` holds at
    /// most `Limits::maxColorAttachments` entries and the pass has at least one attachment (VU-rojo-cmd-0011); every attachment
    /// has the same extent at its mip and every attachment other than a resolve target the same sample count (VU-rojo-cmd-0012);
    /// `area` is not empty and lies within the attachments' extent (VU-rojo-cmd-0013); no subresource appears twice
    /// (VU-rojo-cmd-0014); the color formats fit `Limits::maxColorBytesPerSample` (VU-rojo-cmd-0015).
    struct RenderPassDesc
    {
        /// The color attachments, in the order the pipeline's color targets name them.
        std::span<const ColorAttachment> colors;
        /// The depth and stencil attachment; a null texture means none.
        DepthAttachment depth;
        /// The rectangle rendered to, in pixels.
        Rect2D area;
        /// The label of the pass's scope.
        std::string_view label;
    };
} // namespace rojoRHI
