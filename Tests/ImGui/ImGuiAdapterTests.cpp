//----------------------------------------------------------------------------------------------------------------------
/// @file ImGuiAdapterTests.cpp
/// @brief Tests the ImGui adapter's frame buffer, initialization, texture requests and draw recording on the configured backend.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/ImGuiAdapter.h>
#include <rojoRHI/RHI.h>

#include "FrameRing.h"

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstdint>
#include <functional>
#include <span>

using namespace rojoRHI;
using imgui::detail::FrameRing;

namespace {
#ifdef NDEBUG
    constexpr bool kValidation = false;
#else
    constexpr bool kValidation = true;
#endif

    /// The display and offscreen target edge, in pixels.
    constexpr uint32_t kDisplaySize = 128;

    /// A device, an ImGui context and an offscreen `RGBA8Unorm` target the frames render into.
    class Fixture
    {
      public:
        /// Creates the device (validating in Debug builds), the context and the target.
        Fixture();

        /// Destroys the target, the context and the device.
        ~Fixture();

        /// Runs one host frame in the documented order: the first callback between `ImGui::NewFrame` and `ImGui::Render`, then
        /// the second on the draw data unless it is empty, then a device frame that prepares and renders it into `target`.
        void frame(const std::function<void()>&, const std::function<void(ImDrawData*)>&);

        Device device;
        ImGuiContext* context = nullptr;
        Texture target;
    };

    //==================================================================================================================
    Fixture::Fixture()
    {
        device = *createDevice(DeviceDesc{ .validation = kValidation, .label = "imgui.device" });
        context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(float(kDisplaySize), float(kDisplaySize));
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        target = *device.createTexture(
            TextureDesc{ .format = Format::RGBA8Unorm,
                         .extent = { kDisplaySize, kDisplaySize, 1 },
                         .usage = TextureUsage::RenderTarget | TextureUsage::Sampled,
                         .label = "imgui.target" }
        );
    }

    //==================================================================================================================
    Fixture::~Fixture()
    {
        device.destroy(target);
        ImGui::DestroyContext(context);
        destroyDevice(device);
    }

    //==================================================================================================================
    void Fixture::frame(const std::function<void()>& draw, const std::function<void(ImDrawData*)>& beforePrepare)
    {
        imgui::newFrame();
        ImGui::NewFrame();
        draw();
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        if (beforePrepare)
        {
            beforePrepare(drawData);
        }

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
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kDisplaySize, kDisplaySize }, .label = "imgui.pass" });
        imgui::render(list, drawData);
        list.endRenderPass();
        list.end();
        device.queue(QueueType::Graphics).submit(std::span(&list, 1));
        device.endFrame();
    }

    //==================================================================================================================
    void drawWindow()
    {
        ImGui::Begin("adapter");
        ImGui::Text("rojoRHI::imgui");
        ImGui::End();
    }
} // namespace

//======================================================================================================================
TEST_CASE("three writing frames use three buffers and the fourth reuses the first", "[imgui]")
{
    Device device = *createDevice(DeviceDesc{ .validation = kValidation, .label = "imgui.device" });
    FrameRing ring(device, "imgui.ring");
    std::array<Buffer, 4> buffers{};
    for (Buffer& buffer : buffers)
    {
        ring.newFrame();
        const FrameRing::Allocation allocation = ring.allocate(64, 16);
        CHECK(allocation.offset == 0);
        buffer = allocation.buffer;
    }
    CHECK(buffers[0] != buffers[1]);
    CHECK(buffers[1] != buffers[2]);
    CHECK(buffers[0] != buffers[2]);
    CHECK(buffers[3] == buffers[0]);
    ring.destroy();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a frame that writes nothing does not advance the ring", "[imgui]")
{
    Device device = *createDevice(DeviceDesc{ .validation = kValidation, .label = "imgui.device" });
    FrameRing ring(device, "imgui.ring");
    ring.newFrame();
    const Buffer first = ring.allocate(64, 16).buffer;
    ring.newFrame();
    ring.newFrame();
    const Buffer third = ring.allocate(64, 16).buffer;
    ring.newFrame();
    const Buffer fourth = ring.allocate(64, 16).buffer;
    ring.newFrame();
    const Buffer fifth = ring.allocate(64, 16).buffer;
    CHECK(third != first);
    CHECK(fourth != first);
    CHECK(fourth != third);
    CHECK(fifth == first);
    ring.destroy();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a request that does not fit moves to a larger buffer at offset 0", "[imgui]")
{
    Device device = *createDevice(DeviceDesc{ .validation = kValidation, .label = "imgui.device" });
    FrameRing ring(device, "imgui.ring");
    ring.newFrame();
    const FrameRing::Allocation small = ring.allocate(64, 16);
    const FrameRing::Allocation large = ring.allocate(4 * 1024 * 1024, 16);
    CHECK(large.buffer != small.buffer);
    CHECK(large.offset == 0);
    ring.destroy();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("init, three frames and shutdown", "[imgui]")
{
    Fixture fixture;
    REQUIRE(imgui::init(imgui::InitDesc{ .device = fixture.device, .colorFormat = Format::RGBA8Unorm }).has_value());
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiBackendFlags flags = ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports;
    CHECK((io.BackendFlags & flags) == flags);
    CHECK(io.BackendRendererUserData != nullptr);

    ImTextureData* atlas = nullptr;
    for (int frame = 0; frame < 3; ++frame)
    {
        fixture.frame(drawWindow, [&](ImDrawData*) { atlas = io.Fonts->TexData; });
        REQUIRE(atlas != nullptr);
        CHECK(atlas->Status == ImTextureStatus_OK);
        CHECK(atlas->TexID != ImTextureID_Invalid);
    }

    imgui::shutdown();
    CHECK(io.BackendRendererUserData == nullptr);
    CHECK((io.BackendFlags & flags) == 0);
    CHECK(atlas->TexID == ImTextureID_Invalid);
    CHECK(atlas->BackendUserData == nullptr);
    // ImTextureData::SetStatus turns Destroyed into WantCreate for a texture that keeps its pixels and was not scheduled for
    // destruction, so a renderer initialized again recreates the atlas.
    CHECK(atlas->Status == ImTextureStatus_WantCreate);
}

//======================================================================================================================
TEST_CASE("textureId names view 0 with a valid ImTextureID", "[imgui]")
{
    CHECK(imgui::textureId(ViewIndex{ 0 }) != ImTextureID_Invalid);
    CHECK(imgui::viewIndex(imgui::textureId(ViewIndex{ 0 })) == ViewIndex{ 0 });
    CHECK(imgui::viewIndex(imgui::textureId(ViewIndex{ 7 })) == ViewIndex{ 7 });
}

//======================================================================================================================
TEST_CASE("a draw list larger than the frame buffer grows it within the frame", "[imgui]")
{
    Fixture fixture;
    REQUIRE(imgui::init(imgui::InitDesc{ .device = fixture.device, .colorFormat = Format::RGBA8Unorm }).has_value());
    bool sawVertexOffset = false;
    const auto drawRects = [] {
        ImDrawList* background = ImGui::GetBackgroundDrawList();
        // 17 500 rectangles of four vertices each: 70 000 vertices, more than 16-bit indices reach from one base. The background
        // list is the first one drawn, so its 1 400 000 bytes of vertices size the slot's first buffer (rounded up to 64 KiB, 41 792
        // bytes spare) and its 210 000 bytes of indices then move to a larger buffer: the draws read indices from another buffer
        // than the vertices.
        for (int i = 0; i < 17500; ++i)
        {
            const float x = float(i % 120);
            const float y = float((i / 120) % 120);
            background->AddRectFilled(ImVec2(x, y), ImVec2(x + 2.0f, y + 2.0f), IM_COL32(255, 0, 0, 255));
        }
    };
    const auto findVertexOffset = [&](ImDrawData* drawData) {
        for (const ImDrawList* drawList : drawData->CmdLists)
        {
            for (const ImDrawCmd& cmd : drawList->CmdBuffer)
            {
                sawVertexOffset = sawVertexOffset || cmd.VtxOffset > 0;
            }
        }
    };
    fixture.frame(drawRects, findVertexOffset);
    fixture.frame(drawRects, findVertexOffset);
    CHECK(sawVertexOffset);
    imgui::shutdown();
}

//======================================================================================================================
TEST_CASE("a registered texture is created, updated and destroyed as ImGui asks", "[imgui]")
{
    Fixture fixture;
    REQUIRE(imgui::init(imgui::InitDesc{ .device = fixture.device, .colorFormat = Format::RGBA8Unorm }).has_value());
    ImTextureData texture;
    texture.Create(ImTextureFormat_RGBA32, 4, 4);
    ImGui::RegisterUserTexture(&texture);
    const auto nothing = [] {};

    fixture.frame(nothing, nullptr);
    CHECK(texture.Status == ImTextureStatus_OK);
    CHECK(texture.TexID != ImTextureID_Invalid);

    fixture.frame(nothing, [&](ImDrawData*) {
        const ImTextureRect whole{ 0, 0, 4, 4 };
        texture.Updates.push_back(whole);
        texture.UpdateRect = whole;
        texture.SetStatus(ImTextureStatus_WantUpdates);
    });
    CHECK(texture.Status == ImTextureStatus_OK);

    // ImGui schedules a destroy with WantDestroyNextFrame and counts the frames the texture then goes unused.
    fixture.frame(nothing, [&](ImDrawData*) {
        texture.WantDestroyNextFrame = true;
        texture.SetStatus(ImTextureStatus_WantDestroy);
        texture.UnusedFrames = 0;
    });
    CHECK(texture.Status == ImTextureStatus_WantDestroy);
    CHECK(texture.TexID != ImTextureID_Invalid);

    fixture.frame(nothing, [&](ImDrawData*) { texture.UnusedFrames = 1; });
    CHECK(texture.Status == ImTextureStatus_Destroyed);
    CHECK(texture.TexID == ImTextureID_Invalid);

    ImGui::UnregisterUserTexture(&texture);
    imgui::shutdown();
}

//======================================================================================================================
TEST_CASE("an update of several rectangles records valid usage on a validating device", "[imgui]")
{
    // ImGui queues several rectangles in one update when it adds glyphs or rebuilds the atlas at a new density; copying
    // them one after another into the texture with no barrier between is a write after write the validation tracker aborts
    // on (Debug builds).
    Fixture fixture;
    REQUIRE(imgui::init(imgui::InitDesc{ .device = fixture.device, .colorFormat = Format::RGBA8Unorm }).has_value());
    ImTextureData texture;
    texture.Create(ImTextureFormat_RGBA32, 16, 16);
    ImGui::RegisterUserTexture(&texture);
    const auto nothing = [] {};
    fixture.frame(nothing, nullptr);
    REQUIRE(texture.Status == ImTextureStatus_OK);

    fixture.frame(nothing, [&](ImDrawData*) {
        texture.Updates.push_back(ImTextureRect{ 0, 0, 4, 4 });
        texture.Updates.push_back(ImTextureRect{ 8, 8, 4, 4 });
        texture.Updates.push_back(ImTextureRect{ 12, 0, 2, 2 });
        texture.UpdateRect = ImTextureRect{ 0, 0, 14, 12 };
        texture.SetStatus(ImTextureStatus_WantUpdates);
    });
    CHECK(texture.Status == ImTextureStatus_OK);

    ImGui::UnregisterUserTexture(&texture);
    imgui::shutdown();
}
