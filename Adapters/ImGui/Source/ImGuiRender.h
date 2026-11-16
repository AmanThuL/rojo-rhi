//----------------------------------------------------------------------------------------------------------------------
/// @file ImGuiRender.h
/// @brief Declares the adapter state and the draw-data recording the main viewport and the platform windows share.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/ImGuiAdapter.h>
#include <rojoRHI/RHI.h>

#include "FrameRing.h"

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>

namespace rojoRHI::imgui::detail {
    /// Platform windows present `BGRA8Unorm` with `ColorSpace::SRGB`, the one pair presentation lists for SRGB.
    constexpr Format kPlatformFormat = Format::BGRA8Unorm;

    /// Everything `init` creates, freed by `shutdown`.
    struct State
    {
        Device device;
        std::string label;
        Format colorFormat = Format::BGRA8Unorm;
        SurfaceCallbacks surfaces;
        ShaderLibrary library;
        Pipeline hostPipeline;
        Pipeline platformPipeline; ///< Equal to `hostPipeline` when the host format is the platform windows' format.
        std::optional<SamplerIndex> sampler;
        Fence fence;
        uint64_t fenceValue = 0; ///< The last value a submission signals on `fence`.
        FrameRing ring;
    };

    /// The adapter's state between `init` and `shutdown`, null outside them.
    extern State* g_state;

    /// Prints `message` on stderr and aborts: a creation inside a frame entry failed.
    [[noreturn]] void fatal(const std::string& message);

    /// Records `drawData` into the render pass open on `list` with `pipeline`, whose color format is the pass's, mapping the
    /// display onto the whole `target` extent, which the pass's area covers; scissors are clamped to `target`. Records
    /// nothing when the display size or `target` is not positive or there is no vertex.
    void renderDrawData(CommandList& list, ImDrawData* drawData, Pipeline pipeline, Extent2D target);

    /// Installs the renderer's platform-window callbacks in `ImGui::GetPlatformIO()`.
    void installPlatformWindowCallbacks();

    /// Removes the callbacks `installPlatformWindowCallbacks` installed.
    void removePlatformWindowCallbacks();

    /// Resizes every platform window's swapchain whose extent differs from its framebuffer size, read from the platform's
    /// `Platform_GetWindowSize` and `Platform_GetWindowFramebufferScale` where it installs them, else from the viewport. No
    /// frame is open.
    void resizePlatformWindows();

    /// The extent `viewport`'s swapchain was last resized to, zero for a viewport without one or before its first resize.
    Extent2D platformWindowExtent(ImGuiViewport* viewport);

    /// The presents of `viewport`'s swapchain since its creation, 0 for a viewport without one.
    uint32_t platformWindowPresents(ImGuiViewport* viewport);
} // namespace rojoRHI::imgui::detail
