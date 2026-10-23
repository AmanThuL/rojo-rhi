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
    /// adapter's shader library, creates its pipelines, its sampler and a fence, and sets `io.BackendRendererName`,
    /// `io.BackendRendererUserData` and the flags `ImGuiBackendFlags_RendererHasVtxOffset`, `RendererHasTextures` and
    /// `RendererHasViewports`. A creation failure returns its `Error` after destroying what was created, and leaves the
    /// context untouched. Calling it twice, without a current context, or with a renderer backend already set is an
    /// `IM_ASSERT`. Not thread-safe: the adapter's state is one per process.
    Result<void> init(const InitDesc& desc);

    /// Destroys everything `init` and the frames created: the ImGui textures this adapter created (their status becomes
    /// `ImTextureStatus_Destroyed` and their identifier `ImTextureID_Invalid`), the frame buffers, the pipelines, the
    /// sampler, the shader library and the fence, then clears the renderer name, user data and flags.
    ///
    /// Call outside a device frame, before the platform backend's shutdown and `ImGui::DestroyContext`. Destruction is
    /// deferred by the device, so the call does not wait. Calling it before `init` is an `IM_ASSERT`.
    void shutdown();

    /// Starts an ImGui frame on the renderer side.
    ///
    /// Call once per frame, before `ImGui::NewFrame`, with no device frame open, and at most once per device frame: the
    /// frame buffers advance on the first write after it, so a frame that draws nothing does not consume a slot. It
    /// destroys frame buffers retired by growth in the previous frame. Calling it before `init` is an `IM_ASSERT`.
    void newFrame();

    /// Creates, updates and destroys the textures `drawData->Textures` asks for (`ImGuiBackendFlags_RendererHasTextures`).
    ///
    /// Call once per frame, after `ImGui::Render`, inside the device frame and outside any render pass, before `render`, on
    /// the main viewport's draw data. A new texture is created with its pixels as initial data; an update copies its
    /// rectangles from the frame buffer into the texture, recording the copy and its barriers into `list`; a destroy request
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

    /// Records the passes of every platform window acquired this frame. Call inside the device frame, outside a render pass,
    /// after `render`'s pass ends and before the list is submitted.
    void renderPlatformWindows(CommandList& list);

    /// Presents every platform window `renderPlatformWindows` drew on `queue`, the graphics queue. Call after the submission
    /// of the list it recorded into, before `Device::endFrame`.
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
