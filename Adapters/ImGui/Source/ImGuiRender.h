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
    // Platform windows present it with `ColorSpace::SRGB`, the one pair presentation lists for SRGB.
    constexpr Format kPlatformFormat = Format::BGRA8Unorm;

    struct State
    {
        Device device;
        std::string label;
        Format colorFormat = Format::BGRA8Unorm;
        SurfaceCallbacks surfaces;
        ShaderLibrary library;
        Pipeline hostPipeline;
        Pipeline platformPipeline; // Equal to `hostPipeline` when the host format is the platform windows' format.
        std::optional<SamplerIndex> sampler;
        Fence fence;
        uint64_t fenceValue = 0; // The last value a submission signals on `fence`.
        FrameRing ring;
    };

    // Null outside `init` to `shutdown`.
    extern State* g_state;

    // For a creation failure inside a frame entry, which has no error path: prints `message` on stderr and aborts.
    [[noreturn]] void fatal(const std::string& message);

    // `pipeline`'s color format is the open pass's. The display maps onto the whole of `target`, which the pass's area
    // covers, and scissors are clamped to it. Records nothing when the display size or `target` is not positive or there
    // is no vertex.
    void renderDrawData(CommandList& list, ImDrawData* drawData, Pipeline pipeline, Extent2D target);

    void installPlatformWindowCallbacks();

    void removePlatformWindowCallbacks();

    // Called with no frame open. The framebuffer size comes from the platform's `Platform_GetWindowSize` and
    // `Platform_GetWindowFramebufferScale` where it installs them, else from the viewport.
    void resizePlatformWindows();

    // The extent `viewport`'s swapchain was last resized to; zero without a swapchain or before its first resize.
    Extent2D platformWindowExtent(ImGuiViewport* viewport);

    uint32_t platformWindowPresents(ImGuiViewport* viewport);
} // namespace rojoRHI::imgui::detail
