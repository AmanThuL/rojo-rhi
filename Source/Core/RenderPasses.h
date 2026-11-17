//----------------------------------------------------------------------------------------------------------------------
/// @file RenderPasses.h
/// @brief Declares the checks of the render pass, scope, pipeline binding and dynamic state rules of chapters 06 and 07.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"

#include <string_view>

namespace rojoRHI::core {
    // Dynamic state starts unset and no pipeline is bound; the pass is a scope labeled `desc.label`, or
    // `rojorhi.pass.unnamed`.
    void beginPass(CommandListCore& list, const RenderPassDesc& desc);

    // Unbinds the pipeline.
    void endPass(CommandListCore& list);

    // An empty label records `rojorhi.scope.unnamed`.
    void beginScope(CommandListCore& list, std::string_view label);

    // The scope must have begun on the same side of the pass boundary.
    void endScope(CommandListCore& list);

    void bindPipeline(CommandListCore& list, Pipeline pipeline);

    void setViewport(CommandListCore& list, const Viewport& viewport);

    void setScissor(CommandListCore& list, const Rect2D& rect);

    void setStencilReference(CommandListCore& list);

    void setBlendConstants(CommandListCore& list);

    void setPrimitiveTopology(CommandListCore& list, PrimitiveTopology topology);

    void setDepthBias(CommandListCore& list);
} // namespace rojoRHI::core
