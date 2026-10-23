//----------------------------------------------------------------------------------------------------------------------
/// @file ImGuiViewports.cpp
/// @brief Implements the ImGui platform windows: one swapchain each, resized outside the frame, drawn and presented in it.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/ImGuiAdapter.h>

#include "ImGuiRender.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace rojoRHI::imgui {
    namespace {
        /// The renderer's data of one platform window, held in `ImGuiViewport::RendererUserData`.
        struct ViewportData
        {
            Swapchain swapchain;
            void* surface = nullptr;
            Extent2D extent; ///< The extent last set with `Swapchain::resize`; zero while unknown.
            Texture image;   ///< The image acquired this frame, null otherwise.
            bool acquired = false;
            uint32_t presents = 0; ///< Presents since creation.
            std::string passLabel; ///< The label of the window's render pass.
        };

        /// How long closing a platform window waits for the graphics queue, in nanoseconds.
        constexpr uint64_t kDestroyWaitNs = 5'000'000'000;

        //==============================================================================================================
        ViewportData* dataOf(ImGuiViewport* viewport)
        {
            return static_cast<ViewportData*>(viewport->RendererUserData);
        }

        //==============================================================================================================
        // The platform's own answer is fresher than the flag, which ImGui::NewFrame refreshes after newFrame runs.
        bool isMinimized(const ImGuiPlatformIO& platformIo, ImGuiViewport* viewport)
        {
            if ((viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0)
            {
                return true;
            }
            return viewport->PlatformWindowCreated && platformIo.Platform_GetWindowMinimized != nullptr && platformIo.Platform_GetWindowMinimized(viewport);
        }

        //==============================================================================================================
        // The framebuffer size of a viewport in whole pixels, zero in a dimension that is not positive.
        Extent2D framebufferExtent(ImVec2 size, ImVec2 scale)
        {
            const long width = std::lround(size.x * scale.x);
            const long height = std::lround(size.y * scale.y);
            return Extent2D{ .width = width > 0 ? static_cast<uint32_t>(width) : 0, .height = height > 0 ? static_cast<uint32_t>(height) : 0 };
        }

        //==============================================================================================================
        void createWindow(ImGuiViewport* viewport)
        {
            IM_ASSERT(detail::g_state != nullptr && "a platform window was created without rojoRHI::imgui::init");
            detail::State& state = *detail::g_state;
            IM_ASSERT(state.surfaces.create != nullptr && "ImGuiConfigFlags_ViewportsEnable needs InitDesc::surfaces.create");
            void* surface = state.surfaces.create(viewport, state.surfaces.user);
            IM_ASSERT(surface != nullptr && "SurfaceCallbacks::create returned no surface");
            const std::string label = std::format("{}.viewport", state.label);
            Result<Swapchain> swapchain = state.device.createSwapchain(
                SwapchainDesc{ .nativeSurface = surface,
                               .format = detail::kPlatformFormat,
                               .colorSpace = ColorSpace::SRGB,
                               .imageCount = 3,
                               .mode = PresentMode::Fifo,
                               .maxLatency = 2,
                               .label = label }
            );
            if (!swapchain)
            {
                detail::fatal(std::format("creating the swapchain {} failed: {}", label, swapchain.error().message));
            }
            viewport->RendererUserData = IM_NEW(ViewportData){ .swapchain = *swapchain, .surface = surface, .passLabel = std::format("{}.pass", label) };
        }

        //==============================================================================================================
        // ImGui destroys the platform window, and with it the native surface, right after this call returns, while frames
        // that drew into the swapchain may still be in flight. The surface must outlive the swapchain's destroy and every
        // frame in flight at it, so a submission with no lists signals the fence behind all earlier graphics work, and the
        // call waits for it before the host releases the surface.
        void destroyWindow(ImGuiViewport* viewport)
        {
            ViewportData* data = dataOf(viewport);
            if (data == nullptr)
            {
                return;
            }
            IM_ASSERT(detail::g_state != nullptr && "a platform window outlived rojoRHI::imgui::shutdown");
            IM_ASSERT(!data->acquired && "a platform window was closed between renderPlatformWindows and presentPlatformWindows");
            detail::State& state = *detail::g_state;
            state.device.destroy(data->swapchain);
            const std::array<FenceValue, 1> signal{ FenceValue{ .fence = state.fence, .value = ++state.fenceValue } };
            state.device.queue(QueueType::Graphics).submit({}, {}, signal);
            if (!state.device.wait(signal[0], kDestroyWaitNs))
            {
                detail::fatal(std::format("the graphics queue did not finish within 5 s of closing a platform window of {}", state.label));
            }
            if (state.surfaces.destroy != nullptr)
            {
                state.surfaces.destroy(viewport, data->surface, state.surfaces.user);
            }
            IM_DELETE(data);
            viewport->RendererUserData = nullptr;
        }

        //==============================================================================================================
        // Nothing to do: newFrame reconciles every swapchain's extent, because the OS also resizes platform windows without
        // ImGui calling this.
        void setWindowSize(ImGuiViewport*, ImVec2)
        {
        }
    } // namespace

    //==================================================================================================================
    void detail::installPlatformWindowCallbacks()
    {
        ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        platformIo.Renderer_CreateWindow = createWindow;
        platformIo.Renderer_DestroyWindow = destroyWindow;
        platformIo.Renderer_SetWindowSize = setWindowSize;
    }

    //==================================================================================================================
    void detail::removePlatformWindowCallbacks()
    {
        ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        platformIo.Renderer_CreateWindow = nullptr;
        platformIo.Renderer_DestroyWindow = nullptr;
        platformIo.Renderer_SetWindowSize = nullptr;
    }

    //==================================================================================================================
    void detail::resizePlatformWindows()
    {
        const ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        for (int i = 1; i < platformIo.Viewports.Size; ++i)
        {
            ImGuiViewport* viewport = platformIo.Viewports[i];
            ViewportData* data = dataOf(viewport);
            if (data == nullptr || isMinimized(platformIo, viewport))
            {
                continue;
            }
            const Extent2D want = framebufferExtent(viewport->Size, viewport->FramebufferScale);
            if (want.width == 0 || want.height == 0 || (want.width == data->extent.width && want.height == data->extent.height))
            {
                continue;
            }
            data->swapchain.resize(want);
            data->extent = want;
        }
    }

    //==================================================================================================================
    uint32_t detail::platformWindowPresents(ImGuiViewport* viewport)
    {
        const ViewportData* data = dataOf(viewport);
        return data != nullptr ? data->presents : 0;
    }

    //==================================================================================================================
    void renderPlatformWindows(CommandList& list)
    {
        IM_ASSERT(detail::g_state != nullptr && "rojoRHI::imgui::renderPlatformWindows before init");
        const detail::State& state = *detail::g_state;
        const ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        for (int i = 1; i < platformIo.Viewports.Size; ++i)
        {
            ImGuiViewport* viewport = platformIo.Viewports[i];
            ViewportData* data = dataOf(viewport);
            if (data == nullptr || (viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0 || data->extent.width == 0 || data->extent.height == 0 ||
                viewport->DrawData == nullptr)
            {
                continue;
            }
            IM_ASSERT(!data->acquired && "renderPlatformWindows ran twice without presentPlatformWindows");
            const Texture image = data->swapchain.acquire();
            if (image == Texture{})
            {
                continue;
            }
            const std::array<TextureBarrier, 1> toAttachment{ TextureBarrier{ .texture = image,
                                                                              .dstStage = Stage::ColorAttachment,
                                                                              .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                                                                              .oldLayout = Layout::Undefined,
                                                                              .newLayout = Layout::ColorAttachment } };
            list.barrier({}, {}, toAttachment);
            // An acquired image has undefined contents, so a window that draws its own background need not clear it.
            const bool clear = (viewport->Flags & ImGuiViewportFlags_NoRendererClear) == 0;
            const std::array<ColorAttachment, 1> colors{ ColorAttachment{
                .texture = image, .load = clear ? LoadOp::Clear : LoadOp::DontCare, .store = StoreOp::Store, .clear = ClearColor{ .a = 1.0f } } };
            list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, data->extent.width, data->extent.height }, .label = data->passLabel });
            detail::renderDrawData(list, viewport->DrawData, state.platformPipeline, data->extent);
            list.endRenderPass();
            const std::array<TextureBarrier, 1> toPresent{ TextureBarrier{ .texture = image,
                                                                           .srcStage = Stage::ColorAttachment,
                                                                           .srcAccess = Access::ColorAttachmentWrite,
                                                                           .oldLayout = Layout::ColorAttachment,
                                                                           .newLayout = Layout::Present } };
            list.barrier({}, {}, toPresent);
            data->image = image;
            data->acquired = true;
        }
    }

    //==================================================================================================================
    void presentPlatformWindows(Queue queue)
    {
        IM_ASSERT(detail::g_state != nullptr && "rojoRHI::imgui::presentPlatformWindows before init");
        const ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        for (int i = 1; i < platformIo.Viewports.Size; ++i)
        {
            ViewportData* data = dataOf(platformIo.Viewports[i]);
            if (data == nullptr || !data->acquired)
            {
                continue;
            }
            queue.present(data->swapchain);
            ++data->presents;
            data->image = Texture{};
            data->acquired = false;
        }
    }
} // namespace rojoRHI::imgui
