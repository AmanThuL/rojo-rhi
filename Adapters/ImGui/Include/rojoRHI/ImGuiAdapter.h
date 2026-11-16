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
        /// Returns the native surface for `viewport` (a `CA::MetalLayer*` on Metal 4, an `HWND` on D3D12), which the host owns.
        /// Called from inside ImGui's platform-window update, outside a device frame.
        void* (*create)(ImGuiViewport* viewport, void* user) = nullptr;

        /// Releases `surface`, returned by `create` for `viewport`, once the GPU has finished with the window's swapchain.
        /// Called once per closed platform window, never for the main viewport.
        void (*destroy)(ImGuiViewport* viewport, void* surface, void* user) = nullptr;

        void* user = nullptr; ///< Passed to both callbacks unchanged.
    };

    /// What `init` needs.
    struct InitDesc
    {
        Device device;                            ///< The device every object is created on; it outlives `shutdown`.
        Format colorFormat = Format::BGRA8Unorm;  ///< The color format of the host render pass `render` records into.
        SurfaceCallbacks surfaces;                ///< Required once `ImGuiConfigFlags_ViewportsEnable` creates a platform window.
        std::string_view label = "rojorhi.imgui"; ///< Prefix of every GPU object's label; copied by `init`.
    };

    /// Initializes the renderer for the current ImGui context.
    ///
    /// Call once, after `ImGui::CreateContext` and the platform backend's init, outside a device frame. It loads the
    /// adapter's shader library, creates its pipelines, its sampler and a fence, sets `io.BackendRendererName`,
    /// `io.BackendRendererUserData` and the flags `ImGuiBackendFlags_RendererHasVtxOffset`, `RendererHasTextures` and
    /// `RendererHasViewports`, and installs `Renderer_CreateWindow`, `Renderer_DestroyWindow` and `Renderer_SetWindowSize`
    /// in `ImGuiPlatformIO`. A creation failure returns its `Error` after destroying what was created, and leaves the
    /// context untouched. Calling it twice, without a current context, or with a renderer backend already set is an
    /// `IM_ASSERT`. Not thread-safe: the adapter's state is one per process.
    Result<void> init(const InitDesc& desc);

    /// Destroys everything `init` and the frames created. It first calls `ImGui::DestroyPlatformWindows`, which closes every
    /// platform window as described for `presentPlatformWindows`, then destroys the ImGui textures this adapter created
    /// (their identifier becomes `ImTextureID_Invalid` and their status `ImTextureStatus_Destroyed`, except that ImGui turns
    /// a texture that keeps its pixels and was not scheduled for destruction, such as the font atlas, back to
    /// `ImTextureStatus_WantCreate`, so a later `init` recreates it), the frame buffers, the pipelines, the sampler, the
    /// shader library and the fence, and clears the renderer name, user data, flags and platform-window callbacks.
    ///
    /// Call outside a device frame, while the device is alive, before `ImGui::DestroyContext`; it works before or after the
    /// platform backend's shutdown. Apart from one wait per open platform window, destruction is deferred by the device and
    /// the call does not wait. Calling it before `init` is an `IM_ASSERT`.
    void shutdown();

    /// Starts an ImGui frame on the renderer side.
    ///
    /// Call once per frame, before `ImGui::NewFrame`, with no device frame open, and at most once per device frame: the
    /// frame buffers advance on the first write after it, so a frame that draws nothing does not consume a slot. It
    /// destroys frame buffers retired by growth in the previous frame, and resizes the swapchain of every platform window
    /// whose framebuffer size changed, skipping a minimized window and a zero size; that resize waits until the graphics
    /// queue has finished with the window's images. The size is the platform's `Platform_GetWindowSize` times its
    /// `Platform_GetWindowFramebufferScale`, rounded, read now because the viewport's `Size` and `FramebufferScale` lag the
    /// OS until `ImGui::NewFrame`; the viewport's values stand in for a callback the platform does not install. Calling it
    /// before `init` is an `IM_ASSERT`.
    void newFrame();

    /// Creates, updates and destroys the textures `drawData->Textures` asks for (`ImGuiBackendFlags_RendererHasTextures`).
    ///
    /// Call once per frame, after `ImGui::Render`, inside the device frame and outside any render pass, before `render`, on
    /// the main viewport's draw data. A new texture is created with its pixels as initial data; an update copies the
    /// bounding box of its rectangles from the frame buffer into the texture, recording the copy and its barriers into `list`; a destroy request
    /// is honoured once the texture went unused for a frame. Creating a texture or growing the frame buffer is fatal on
    /// failure: a message on stderr, then `std::abort()`. A texture format other than `ImTextureFormat_RGBA32` is an
    /// `IM_ASSERT`.
    void prepare(CommandList& list, ImDrawData* drawData);

    /// Records `drawData` into the render pass the host opened on `list`, whose single color attachment has the
    /// `InitDesc::colorFormat` format.
    ///
    /// Call inside the device frame, after `prepare`, between `beginRenderPass` and `endRenderPass`. It sets the pipeline,
    /// the viewport (the whole display), the topology, a scissor per command and a root per command, and draws each command
    /// with `drawIndexed`. Vertices and indices are copied into the frame buffer, valid for the frame. The pipeline, viewport
    /// and scissor are left changed. It records nothing when the display size is not positive or there is no vertex. Growing
    /// the frame buffer is fatal on failure.
    void render(CommandList& list, ImDrawData* drawData);

    /// Acquires and draws every platform window into its own swapchain (`BGRA8Unorm`, `ColorSpace::SRGB`).
    ///
    /// Call once per frame, inside the device frame, outside a render pass, after `render`'s pass ends and before `list` is
    /// submitted to the graphics queue. For each platform window that is not flagged `ImGuiViewportFlags_IsMinimized`, has a
    /// known non-zero extent and has draw data, it acquires an image, records a barrier from `Layout::Undefined`, one render
    /// pass that clears to opaque black (left undefined under `ImGuiViewportFlags_NoRendererClear`) and draws the window over
    /// the whole image, and a barrier to `Layout::Present`. A window resized since `newFrame` is drawn stretched into the
    /// extent `newFrame` set, which catches up at the next `newFrame`. A window whose acquire returns a null texture is
    /// skipped this frame. A platform window created in one frame is first drawn in the next.
    ///
    /// Each window's swapchain is created by ImGui's `Renderer_CreateWindow`, on the surface `SurfaceCallbacks::create`
    /// returns; a creation failure is fatal. Closing a window (`Renderer_DestroyWindow`) destroys its swapchain, signals the
    /// adapter's fence from a submission with no lists on the graphics queue, waits for it (fatal after 5 s), then calls
    /// `SurfaceCallbacks::destroy`, so the surface outlives every frame that drew into it. ImGui closes windows in
    /// `ImGui::UpdatePlatformWindows`, which the host therefore calls after `Device::endFrame`, outside any frame.
    void renderPlatformWindows(CommandList& list);

    /// Presents every platform window `renderPlatformWindows` drew on `queue`, the graphics queue. Call after the submission
    /// of the list it recorded into, after the host's own present and before `Device::endFrame`.
    void presentPlatformWindows(Queue queue);

    /// Returns the `ImTextureID` naming the host view `view`, for `ImGui::Image` and the draw list's image calls.
    ///
    /// The identifier is the view index plus one, because `ImTextureID_Invalid` is 0 and view index 0 is a valid view. The
    /// host keeps the view's texture in `Layout::ShaderRead` while any draw data names it. `Device::destroy` of the view is
    /// deferred, so the host only never destroys it while a list that is not yet submitted references it. Usable at any time,
    /// before `init` too.
    ImTextureID textureId(ViewIndex view);

    /// Returns the view index an identifier from `textureId`, or one this adapter gave an ImGui texture, names.
    ViewIndex viewIndex(ImTextureID id);
} // namespace rojoRHI::imgui
