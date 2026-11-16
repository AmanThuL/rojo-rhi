//----------------------------------------------------------------------------------------------------------------------
/// @file ImGuiViewportTests.cpp
/// @brief Tests the ImGui adapter's platform windows, one swapchain each, over a fake platform backend on the configured backend.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/ImGuiAdapter.h>
#include <rojoRHI/RHI.h>

#include "ImGuiRender.h"
#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
#ifdef NDEBUG
    constexpr bool kValidation = false;
#else
    constexpr bool kValidation = true;
#endif

    /// The main display's edge and every platform window's default edge, in pixels: Metal 4's shared surfaces are as large.
    constexpr uint32_t kEdge = 256;

    /// A window of the fake platform. Index 0 is the main viewport's.
    struct FakeWindow
    {
        ImVec2 pos;
        ImVec2 size;
        bool minimized = false;
        bool open = true;
    };

    /// The fake platform's windows; a viewport's `PlatformHandle` is its window's index plus one, so none is null.
    std::vector<FakeWindow> g_windows;

    /// The framebuffer scale the fake platform reports for every window; a case changes it as an OS would when a window
    /// moves to a denser display.
    ImVec2 g_framebufferScale(1.0f, 1.0f);

    /// What the surface callbacks observed.
    struct SurfaceLog
    {
        int creates = 0;
        std::vector<void*> destroyed;
        bool destroyedMain = false;
    };

    SurfaceLog g_surfaces;

    //==================================================================================================================
    void* handleOf(size_t index)
    {
        return reinterpret_cast<void*>(uintptr_t(index + 1));
    }

    //==================================================================================================================
    size_t indexOf(ImGuiViewport* viewport)
    {
        return size_t(reinterpret_cast<uintptr_t>(viewport->PlatformHandle)) - 1;
    }

    //==================================================================================================================
    FakeWindow& windowOf(ImGuiViewport* viewport)
    {
        return g_windows[indexOf(viewport)];
    }

    //==================================================================================================================
    void platformCreateWindow(ImGuiViewport* viewport)
    {
        g_windows.push_back(FakeWindow{ .pos = viewport->Pos, .size = viewport->Size });
        viewport->PlatformHandle = handleOf(g_windows.size() - 1);
        viewport->PlatformHandleRaw = viewport->PlatformHandle;
    }

    //==================================================================================================================
    void platformDestroyWindow(ImGuiViewport* viewport)
    {
        if (viewport != ImGui::GetMainViewport() && viewport->PlatformHandle != nullptr)
        {
            windowOf(viewport).open = false;
        }
    }

    //==================================================================================================================
    void platformShowWindow(ImGuiViewport*)
    {
    }

    //==================================================================================================================
    void platformSetWindowPos(ImGuiViewport* viewport, ImVec2 pos)
    {
        windowOf(viewport).pos = pos;
    }

    //==================================================================================================================
    ImVec2 platformGetWindowPos(ImGuiViewport* viewport)
    {
        return windowOf(viewport).pos;
    }

    //==================================================================================================================
    void platformSetWindowSize(ImGuiViewport* viewport, ImVec2 size)
    {
        windowOf(viewport).size = size;
    }

    //==================================================================================================================
    ImVec2 platformGetWindowSize(ImGuiViewport* viewport)
    {
        return windowOf(viewport).size;
    }

    //==================================================================================================================
    ImVec2 platformGetWindowFramebufferScale(ImGuiViewport*)
    {
        return g_framebufferScale;
    }

    //==================================================================================================================
    // The pixels of `size` at `scale`, rounded to nearest as the adapter rounds a framebuffer size.
    uint32_t pixels(float size, float scale)
    {
        return uint32_t(std::lround(size * scale));
    }

    //==================================================================================================================
    bool platformGetWindowFocus(ImGuiViewport*)
    {
        return false;
    }

    //==================================================================================================================
    bool platformGetWindowMinimized(ImGuiViewport* viewport)
    {
        return windowOf(viewport).minimized;
    }

    //==================================================================================================================
    void platformSetWindowTitle(ImGuiViewport*, const char*)
    {
    }

    //==================================================================================================================
    // The first platform window (index 1) gets surface 6 and the second surface 7.
    void* createSurface(ImGuiViewport* viewport, void*)
    {
        ++g_surfaces.creates;
        return test::surface(uint32_t(5 + indexOf(viewport)));
    }

    //==================================================================================================================
    void destroySurface(ImGuiViewport* viewport, void* surface, void*)
    {
        g_surfaces.destroyed.push_back(surface);
        g_surfaces.destroyedMain = g_surfaces.destroyedMain || viewport == ImGui::GetMainViewport();
    }

    /// A device, an ImGui context with viewports enabled over the fake platform, the adapter and an offscreen main target.
    class Host
    {
      public:
        /// Creates the device (validating in Debug builds), the context, the fake platform and the target, then the adapter.
        Host();

        /// Shuts the adapter and the fake platform down, then destroys the target, the context and the device.
        ~Host();

        /// Runs one frame in the documented host order, with the callback between `ImGui::NewFrame` and `ImGui::Render`.
        void frame(const std::function<void()>&);

        /// Runs the frame `frame` runs after `imgui::newFrame`, for a case that calls that itself.
        void finishFrame(const std::function<void()>&);

        Device device;
        ImGuiContext* context = nullptr;
        Texture target;
    };

    //==================================================================================================================
    Host::Host()
    {
        g_windows.assign(1, FakeWindow{ .pos = ImVec2(0.0f, 0.0f), .size = ImVec2(float(kEdge), float(kEdge)) });
        g_surfaces = {};
        g_framebufferScale = ImVec2(1.0f, 1.0f);
        device = *createDevice(DeviceDesc{ .validation = kValidation, .label = "imgui.device" });
        context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(float(kEdge), float(kEdge));
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.ConfigViewportsNoAutoMerge = true;
        io.BackendPlatformName = "rojorhi.test";
        io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;

        ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        ImGuiPlatformMonitor monitor;
        monitor.MainSize = monitor.WorkSize = ImVec2(4096.0f, 4096.0f);
        platformIo.Monitors.push_back(monitor);
        platformIo.Platform_CreateWindow = platformCreateWindow;
        platformIo.Platform_DestroyWindow = platformDestroyWindow;
        platformIo.Platform_ShowWindow = platformShowWindow;
        platformIo.Platform_SetWindowPos = platformSetWindowPos;
        platformIo.Platform_GetWindowPos = platformGetWindowPos;
        platformIo.Platform_SetWindowSize = platformSetWindowSize;
        platformIo.Platform_GetWindowSize = platformGetWindowSize;
        platformIo.Platform_GetWindowFramebufferScale = platformGetWindowFramebufferScale;
        platformIo.Platform_GetWindowFocus = platformGetWindowFocus;
        platformIo.Platform_GetWindowMinimized = platformGetWindowMinimized;
        platformIo.Platform_SetWindowTitle = platformSetWindowTitle;
        ImGuiViewport* main = ImGui::GetMainViewport();
        main->PlatformHandle = handleOf(0);
        main->PlatformHandleRaw = main->PlatformHandle;

        target = *device.createTexture(
            TextureDesc{ .format = Format::RGBA8Unorm,
                         .extent = { kEdge, kEdge, 1 },
                         .usage = TextureUsage::RenderTarget | TextureUsage::Sampled,
                         .label = "imgui.target" }
        );
        // The host pass is RGBA8Unorm, so the platform windows use the adapter's second pipeline.
        const Result<void> initialized = imgui::init(
            imgui::InitDesc{ .device = device, .colorFormat = Format::RGBA8Unorm, .surfaces = { .create = createSurface, .destroy = destroySurface } }
        );
        REQUIRE(initialized);
    }

    //==================================================================================================================
    Host::~Host()
    {
        imgui::shutdown();
        ImGuiIO& io = ImGui::GetIO();
        io.BackendPlatformName = nullptr;
        io.BackendFlags &= ~ImGuiBackendFlags_PlatformHasViewports;
        ImGui::GetPlatformIO().ClearPlatformHandlers();
        device.destroy(target);
        ImGui::DestroyContext(context);
        destroyDevice(device);
    }

    //==================================================================================================================
    void Host::frame(const std::function<void()>& draw)
    {
        imgui::newFrame();
        finishFrame(draw);
    }

    //==================================================================================================================
    void Host::finishFrame(const std::function<void()>& draw)
    {
        ImGui::NewFrame();
        draw();
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();

        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "imgui.list");
        const std::array<TextureBarrier, 1> toAttachment{ TextureBarrier{ .texture = target,
                                                                          .srcStage = Stage::ColorAttachment,
                                                                          .dstStage = Stage::ColorAttachment,
                                                                          .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                                                                          .oldLayout = Layout::Undefined,
                                                                          .newLayout = Layout::ColorAttachment } };
        list.barrier({}, {}, toAttachment);
        imgui::prepare(list, drawData);
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear, .store = StoreOp::Store } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kEdge, kEdge }, .label = "imgui.pass" });
        imgui::render(list, drawData);
        list.endRenderPass();
        imgui::renderPlatformWindows(list);
        list.end();
        Queue graphics = device.queue(QueueType::Graphics);
        graphics.submit(std::span(&list, 1));
        imgui::presentPlatformWindows(graphics);
        device.endFrame();
        ImGui::UpdatePlatformWindows();
    }

    //==================================================================================================================
    // One window placed outside the main display, so it gets a viewport of its own.
    void drawWindow(const char* name, float x, ImVec2 size)
    {
        ImGui::SetNextWindowPos(ImVec2(x, 0.0f));
        ImGui::SetNextWindowSize(size);
        ImGui::Begin(name);
        ImGui::Text("%s", name);
        ImGui::End();
    }

    //==================================================================================================================
    void drawBoth()
    {
        drawWindow("one", 1000.0f, ImVec2(float(kEdge), float(kEdge)));
        drawWindow("two", 1400.0f, ImVec2(float(kEdge), float(kEdge)));
    }

    //==================================================================================================================
    // The viewport of the window `name`, which has one of its own.
    ImGuiViewport* viewportOf(const char* name)
    {
        ImGuiWindow* window = ImGui::FindWindowByName(name);
        return window != nullptr ? window->Viewport : nullptr;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a frame with two platform windows renders and presents both", "[imgui]")
{
    Host host;
    host.frame(drawBoth);
    CHECK(g_surfaces.creates == 2);
    REQUIRE(ImGui::GetPlatformIO().Viewports.Size == 3);

    host.frame(drawBoth);
    CHECK(ImGui::GetPlatformIO().Viewports.Size == 3);
    ImGuiViewport* one = viewportOf("one");
    ImGuiViewport* two = viewportOf("two");
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    CHECK(imgui::detail::platformWindowPresents(one) == 1);
    CHECK(imgui::detail::platformWindowPresents(two) == 1);
    CHECK(g_surfaces.destroyed.empty());
}

//======================================================================================================================
TEST_CASE("closing a platform window destroys its surface once", "[imgui]")
{
    {
        Host host;
        host.frame(drawBoth);
        host.frame(drawBoth);
        REQUIRE(g_surfaces.creates == 2);

        host.frame([] { drawWindow("one", 1000.0f, ImVec2(float(kEdge), float(kEdge))); });
        REQUIRE(g_surfaces.destroyed.size() == 1);
        CHECK(g_surfaces.destroyed[0] == test::surface(7));
        CHECK(ImGui::GetPlatformIO().Viewports.Size == 2);
    }
    REQUIRE(g_surfaces.destroyed.size() == 2);
    CHECK(g_surfaces.destroyed[1] == test::surface(6));
    CHECK_FALSE(g_surfaces.destroyedMain);
    CHECK(g_surfaces.creates == 2);
}

//======================================================================================================================
TEST_CASE("a resized or minimized platform window", "[imgui]")
{
    Host host;
    const ImVec2 small(200.0f, 120.0f);
    host.frame(drawBoth);
    host.frame(drawBoth);
    ImGuiViewport* two = viewportOf("two");
    REQUIRE(two != nullptr);
    REQUIRE(imgui::detail::platformWindowPresents(two) == 1);

    // The frame that changes the size draws the window stretched into the old extent; the next newFrame resizes it.
    const auto drawSmall = [small] {
        drawWindow("one", 1000.0f, ImVec2(float(kEdge), float(kEdge)));
        drawWindow("two", 1400.0f, small);
    };
    host.frame(drawSmall);
    host.frame(drawSmall);
    CHECK(two->Size.x == small.x);
    CHECK(two->Size.y == small.y);
    const uint32_t resized = imgui::detail::platformWindowPresents(two);
    CHECK(resized == 3);

    // A minimized window is neither resized, nor acquired, nor presented.
    windowOf(two).minimized = true;
    host.frame(drawSmall);
    host.frame(drawSmall);
    CHECK((two->Flags & ImGuiViewportFlags_IsMinimized) != 0);
    CHECK(imgui::detail::platformWindowPresents(two) == resized);
    CHECK(imgui::detail::platformWindowPresents(viewportOf("one")) == 5);

    // Restored at full size, it is drawn again, and the shared surface is left at the size it was found at.
    windowOf(two).minimized = false;
    host.frame(drawBoth);
    host.frame(drawBoth);
    CHECK(two->Size.x == float(kEdge));
    CHECK(imgui::detail::platformWindowPresents(two) == resized + 2);
    CHECK(g_surfaces.destroyed.empty());
}

//======================================================================================================================
TEST_CASE("a platform window resized on consecutive frames presents every frame", "[imgui]")
{
    Host host;
    host.frame(drawBoth);
    host.frame(drawBoth);
    ImGuiViewport* two = viewportOf("two");
    REQUIRE(two != nullptr);
    REQUIRE(imgui::detail::platformWindowPresents(two) == 1);

    // A drag changes the size in every frame, so the draw data never matches the extent newFrame set.
    const std::array<ImVec2, 3> sizes{ ImVec2(240.0f, 200.0f), ImVec2(220.0f, 160.0f), ImVec2(float(kEdge), float(kEdge)) };
    for (uint32_t frame = 0; frame < sizes.size(); ++frame)
    {
        const ImVec2 size = sizes[frame];
        host.frame([size] {
            drawWindow("one", 1000.0f, ImVec2(float(kEdge), float(kEdge)));
            drawWindow("two", 1400.0f, size);
        });
        CHECK(imgui::detail::platformWindowPresents(two) == frame + 2);
    }
    // The last frame restored the full size; this one resizes the shared surface back to it.
    host.frame(drawBoth);
    CHECK(imgui::detail::platformWindowPresents(two) == 5);
}

//======================================================================================================================
TEST_CASE("newFrame sizes a platform window's swapchain from the platform's size and scale, not the viewport's older ones", "[imgui]")
{
    Host host;
    host.frame(drawBoth);
    host.frame(drawBoth);
    ImGuiViewport* two = viewportOf("two");
    REQUIRE(two != nullptr);
    CHECK(imgui::detail::platformWindowExtent(two).width == kEdge);

    // The OS resized the window and moved it to a denser display: the platform reports both at once, and ImGui::NewFrame
    // has not yet run to copy them into the viewport.
    const ImVec2 dragged(180.0f, 140.0f);
    windowOf(two).size = dragged;
    g_framebufferScale = ImVec2(2.0f, 2.0f);
    imgui::newFrame();
    CHECK(two->Size.x == float(kEdge));
    CHECK(two->FramebufferScale.x == 1.0f);
    const Extent2D extent = imgui::detail::platformWindowExtent(two);
    CHECK(extent.width == pixels(dragged.x, 2.0f));
    CHECK(extent.height == pixels(dragged.y, 2.0f));
    CHECK(extent.width == 360);
    CHECK(extent.height == 280);
    // ImGui::NewFrame then takes the denser scale and rebuilds the font atlas for it, an update of several rectangles.
    host.finishFrame(drawBoth);
    CHECK(two->FramebufferScale.x == 2.0f);
    CHECK(imgui::detail::platformWindowPresents(two) == 2);

    // The platform window keeps the size and scale the OS gave it, and so does its swapchain.
    host.frame(drawBoth);
    CHECK(imgui::detail::platformWindowExtent(two).width == extent.width);
    CHECK(imgui::detail::platformWindowExtent(two).height == extent.height);
    CHECK(imgui::detail::platformWindowPresents(two) == 3);
}

//======================================================================================================================
TEST_CASE("newFrame sizes a platform window's swapchain from the viewport when the platform reports no window size", "[imgui]")
{
    Host host;
    host.frame(drawBoth);
    host.frame(drawBoth);
    ImGuiViewport* two = viewportOf("two");
    REQUIRE(two != nullptr);

    // Without Platform_GetWindowSize and Platform_GetWindowFramebufferScale, the window's own size goes unread: newFrame
    // keeps the viewport's extent. ImGui::NewFrame needs the size callback, so it is restored before that runs.
    windowOf(two).size = ImVec2(180.0f, 140.0f);
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.Platform_GetWindowSize = nullptr;
    platformIo.Platform_GetWindowFramebufferScale = nullptr;
    imgui::newFrame();
    platformIo.Platform_GetWindowSize = platformGetWindowSize;
    platformIo.Platform_GetWindowFramebufferScale = platformGetWindowFramebufferScale;
    const Extent2D extent = imgui::detail::platformWindowExtent(two);
    CHECK(extent.width == pixels(two->Size.x, two->FramebufferScale.x));
    CHECK(extent.height == pixels(two->Size.y, two->FramebufferScale.y));
    CHECK(extent.width == kEdge);
    host.finishFrame(drawBoth);
    CHECK(imgui::detail::platformWindowPresents(two) == 2);
}
