//----------------------------------------------------------------------------------------------------------------------
/// @file ImGuiAdapter.h
/// @brief Declares the backend-neutral Dear ImGui renderer: initialization, the per-frame entries and texture identifiers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <imgui.h>

#include <string_view>

namespace rojoRHI::imgui {
    /// Host callbacks that create and destroy the native surface of an ImGui platform window.
    struct SurfaceCallbacks
    {
        /// Returns the host-owned native surface for `viewport`: a `CA::MetalLayer*` on Metal 4, an `HWND` on D3D12. Called
        /// from ImGui's platform-window update, outside a device frame.
        void* (*create)(ImGuiViewport* viewport, void* user) = nullptr;

        /// Releases the `surface` that `create` returned for `viewport`, once the GPU has finished with the window's
        /// swapchain. Called once per closed platform window, never for the main viewport.
        void (*destroy)(ImGuiViewport* viewport, void* surface, void* user) = nullptr;

        void* user = nullptr; ///< Passed to both callbacks unchanged.
    };

    /// Read by `init`, which copies every field.
    struct InitDesc
    {
        Device device;                            ///< Every object is created on it; it outlives `shutdown`.
        Format colorFormat = Format::BGRA8Unorm;  ///< The color format of the host render pass `render` records into.
        SurfaceCallbacks surfaces;                ///< Required once `ImGuiConfigFlags_ViewportsEnable` creates a platform window.
        std::string_view label = "rojorhi.imgui"; ///< Prefix of every GPU object's label.
    };

    /// Initializes the renderer for the current ImGui context.
    ///
    /// Call once, after `ImGui::CreateContext` and the platform backend's init, outside a device frame. On success it sets
    /// `io.BackendRendererName`, `io.BackendRendererUserData` and the flags `ImGuiBackendFlags_RendererHasVtxOffset`,
    /// `RendererHasTextures` and `RendererHasViewports`, and installs `Renderer_CreateWindow`, `Renderer_DestroyWindow`,
    /// `Renderer_SetWindowSize` and `DrawCallback_ResetRenderState` in `ImGuiPlatformIO`. A failure to create the adapter's
    /// shader library, pipelines, sampler or fence returns its `Error`, destroys what was created and leaves the context
    /// untouched. Calling it twice, without a current context, or with a renderer backend already set is an `IM_ASSERT`. Not
    /// thread-safe: the adapter's state is one per process.
    Result<void> init(const InitDesc& desc);

    /// Destroys everything `init` and the frames created, and clears what `init` set in the ImGui context.
    ///
    /// Call outside a device frame, while the device is alive, before `ImGui::DestroyContext`, before or after the platform
    /// backend's shutdown. It first calls `ImGui::DestroyPlatformWindows`, which closes every platform window as
    /// `renderPlatformWindows` describes, with one wait per open window; every other destruction is deferred by the device.
    /// Each ImGui texture the adapter created gets `ImTextureID_Invalid` and `ImTextureStatus_Destroyed`, except that ImGui
    /// turns one that keeps its pixels and was not scheduled for destruction, such as the font atlas, back to
    /// `ImTextureStatus_WantCreate`, so a later `init` recreates it. Calling it before `init` is an `IM_ASSERT`.
    void shutdown();

    /// Starts an ImGui frame on the renderer side.
    ///
    /// Call once per frame, before `ImGui::NewFrame`, with no device frame open, and at most once per device frame. The frame
    /// buffers advance on the first write after it, so a frame that draws nothing consumes no slot. It destroys the frame
    /// buffers growth retired in the previous frame, and resizes the swapchain of every platform window whose framebuffer
    /// size changed, skipping a minimized window and a zero size; a resize waits until the graphics queue has finished with
    /// the window's images. The size is `Platform_GetWindowSize` times `Platform_GetWindowFramebufferScale`, rounded, read
    /// now because the viewport's `Size` and `FramebufferScale` lag the OS until `ImGui::NewFrame`; the viewport's values
    /// stand in for a callback the platform does not install. Calling it before `init` is an `IM_ASSERT`.
    void newFrame();

    /// Creates, updates and destroys the textures `drawData->Textures` asks for (`ImGuiBackendFlags_RendererHasTextures`).
    ///
    /// Call once per frame on the main viewport's draw data, after `ImGui::Render` and before `render`, inside the device
    /// frame and outside any render pass. A new texture is created with its pixels as initial data. An update records into
    /// `list` one copy of the bounding box of its rectangles from the frame buffer, between two barriers. A destroy request
    /// is honoured once the texture has gone unused for a frame. Failing to create a texture or to grow the frame buffer is
    /// fatal: a message on stderr, then `std::abort()`. A texture format other than `ImTextureFormat_RGBA32` is an
    /// `IM_ASSERT`.
    void prepare(CommandList& list, ImDrawData* drawData);

    /// Records `drawData` into the render pass the host opened on `list`, whose single color attachment has the
    /// `InitDesc::colorFormat` format.
    ///
    /// Call inside the device frame, after `prepare`, between `beginRenderPass` and `endRenderPass`. The viewport covers the
    /// whole display; each command gets its own scissor and root and one `drawIndexed`. Vertices and indices are copied into
    /// the frame buffer, valid for the frame. The pipeline, viewport, topology and scissor are left changed. Records nothing
    /// when the display size is not positive or there is no vertex. Failing to grow the frame buffer is fatal.
    void render(CommandList& list, ImDrawData* drawData);

    /// Acquires and draws every platform window into its own swapchain (`BGRA8Unorm`, `ColorSpace::SRGB`).
    ///
    /// Call once per frame, inside the device frame and outside a render pass, after `render`'s pass ends and before `list`
    /// is submitted to the graphics queue. Each platform window that is not flagged `ImGuiViewportFlags_IsMinimized`, has a
    /// known non-zero extent and has draw data gets an acquired image, a barrier from `Layout::Undefined`, one render pass
    /// over the whole image that clears to opaque black (left undefined under `ImGuiViewportFlags_NoRendererClear`), and a
    /// barrier to `Layout::Present`. A window resized since `newFrame` is drawn stretched into the extent `newFrame` set,
    /// until the next `newFrame`. A window whose acquire returns a null texture is skipped this frame; one created in a frame
    /// is first drawn in the next.
    ///
    /// ImGui's `Renderer_CreateWindow` creates each window's swapchain on the surface `SurfaceCallbacks::create` returns; a
    /// creation failure is fatal. Closing a window (`Renderer_DestroyWindow`) destroys its swapchain, signals the adapter's
    /// fence from a submission with no lists on the graphics queue, waits for it (fatal after 5 s), then calls
    /// `SurfaceCallbacks::destroy`, so the surface outlives every frame that drew into it. ImGui closes windows in
    /// `ImGui::UpdatePlatformWindows`, so the host calls it after `Device::endFrame`, outside any frame.
    void renderPlatformWindows(CommandList& list);

    /// Presents on `queue`, the graphics queue, every platform window `renderPlatformWindows` drew. Call after submitting the
    /// list it recorded into, after the host's own present and before `Device::endFrame`.
    void presentPlatformWindows(Queue queue);

    /// Returns the `ImTextureID` naming the host view `view`, for `ImGui::Image` and the draw list's image calls.
    ///
    /// The identifier is the view index plus one, because `ImTextureID_Invalid` is 0 and view index 0 is valid. The host
    /// keeps the view's texture in `Layout::ShaderRead` while any draw data names it, and does not destroy the view while a
    /// list not yet submitted references it (`Device::destroy` defers past submitted work). Usable at any time, before
    /// `init` too.
    ImTextureID textureId(ViewIndex view);

    /// Inverts `textureId`, for its identifiers and for those this adapter gave its ImGui textures.
    ViewIndex viewIndex(ImTextureID id);
} // namespace rojoRHI::imgui
