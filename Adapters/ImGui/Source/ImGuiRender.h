//----------------------------------------------------------------------------------------------------------------------
/// @file ImGuiRender.h
/// @brief Declares the draw-data recording the main viewport and the platform windows share.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <imgui.h>

namespace rojoRHI::imgui::detail {
    /// Records `drawData` into the render pass open on `list` with `pipeline`, whose color format is the pass's. Records
    /// nothing when the display size is not positive or there is no vertex.
    void renderDrawData(CommandList& list, ImDrawData* drawData, Pipeline pipeline);
} // namespace rojoRHI::imgui::detail
