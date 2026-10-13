//----------------------------------------------------------------------------------------------------------------------
/// @file RenderPasses.h
/// @brief Declares the checks of the render pass, scope, pipeline binding and dynamic state rules of chapters 06 and 07.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Geometry.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/PrimitiveTopology.h>
#include <rojoRHI/RenderPassDesc.h>
#include <rojoRHI/Viewport.h>

#include "Core/CommandLists.h"

#include <string_view>

namespace rojoRHI::core {
    /// Checks that `list` can begin the pass `desc` describes (VU-rojo-cmd-0063, VU-rojo-cmd-0005, 0008, 0009, 0011 to
    /// 0016, 0071, VU-rojo-format-0003 for a resolve target, VU-rojo-core-0001 to 0003 for the textures) and opens it: dynamic state starts unset and no
    /// pipeline is bound. The pass forms a scope labeled `desc.label`, or `rojorhi.pass.unnamed` when empty.
    void beginPass(CommandListCore& list, const RenderPassDesc& desc);

    /// Checks that a pass is open and every scope begun in it has ended (VU-rojo-cmd-0063, 0018, 0019), then closes it and
    /// unbinds the pipeline.
    void endPass(CommandListCore& list);

    /// Opens a scope on `list` (VU-rojo-cmd-0063), inside or outside a pass, and records it with `label`, or
    /// `rojorhi.scope.unnamed` when empty.
    void beginScope(CommandListCore& list, std::string_view label);

    /// Checks that a scope is open and began on the same side of the pass boundary (VU-rojo-cmd-0063, 0032, 0033), then
    /// closes it.
    void endScope(CommandListCore& list);

    /// Binds `pipeline` (VU-rojo-cmd-0063, 0020 to 0022): a graphics pipeline inside a pass that matches the pass's attachment
    /// formats and sample count, a compute pipeline outside a pass on a list that accepts compute.
    void bindPipeline(CommandListCore& list, Pipeline pipeline);

    /// Checks and records a viewport (VU-rojo-cmd-0063, VU-rojo-pipeline-0015 to 0017).
    void setViewport(CommandListCore& list, const Viewport& viewport);

    /// Checks and records a scissor rectangle (VU-rojo-cmd-0063, VU-rojo-pipeline-0018, 0019).
    void setScissor(CommandListCore& list, const Rect2D& rect);

    /// Checks a stencil reference command (VU-rojo-cmd-0063, VU-rojo-pipeline-0020).
    void setStencilReference(CommandListCore& list);

    /// Checks a blend constants command (VU-rojo-cmd-0063, VU-rojo-pipeline-0021).
    void setBlendConstants(CommandListCore& list);

    /// Checks a topology command (VU-rojo-cmd-0063, VU-rojo-pipeline-0022, 0023).
    void setPrimitiveTopology(CommandListCore& list, PrimitiveTopology topology);

    /// Checks a depth bias command (VU-rojo-cmd-0063, VU-rojo-pipeline-0024, 0025).
    void setDepthBias(CommandListCore& list);
} // namespace rojoRHI::core
