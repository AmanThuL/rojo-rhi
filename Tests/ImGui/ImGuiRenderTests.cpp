//----------------------------------------------------------------------------------------------------------------------
/// @file ImGuiRenderTests.cpp
/// @brief Compares every pixel the ImGui adapter draws for a fixed draw list against an expectation built on the CPU.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/ImGuiAdapter.h>
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace rojoRHI;

namespace {
    constexpr uint32_t kWidth = 96;
    constexpr uint32_t kHeight = 64;

    /// One RGBA8 texel.
    struct Texel
    {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 0;
    };

    /// A rectangle covering `[x0, x1) x [y0, y1)` in one color.
    struct Rect
    {
        uint32_t x0, y0, x1, y1;
        Texel color;
    };

    /// Returns the expected image: transparent black, then each rectangle in order.
    //==================================================================================================================
    std::vector<Texel> expectation(std::span<const Rect> rects)
    {
        std::vector<Texel> image(size_t{ kWidth } * kHeight);
        for (const Rect& rect : rects)
        {
            for (uint32_t y = rect.y0; y < rect.y1; ++y)
            {
                for (uint32_t x = rect.x0; x < rect.x1; ++x)
                {
                    image[size_t{ y } * kWidth + x] = rect.color;
                }
            }
        }
        return image;
    }

    /// Fills every texel of a 4x4 RGBA32 `texture` with `color`.
    //==================================================================================================================
    void fill(ImTextureData& texture, Texel color)
    {
        for (int i = 0; i < texture.Width * texture.Height; ++i)
        {
            std::memcpy(static_cast<uint8_t*>(texture.GetPixels()) + size_t(i) * 4, &color, sizeof(color));
        }
    }

    /// Records one frame of the fixed draw list into `target` and runs it; `beforePrepare` runs on the finished draw data.
    //==================================================================================================================
    template<class Before> void drawFrame(test::GpuKit& kit, Texture target, ImTextureID blue, ImTextureData& green, Before beforePrepare)
    {
        // No platform window exists, so newFrame resizes nothing and may run with the kit's frame already open.
        imgui::newFrame();
        ImGui::NewFrame();
        ImDrawList* background = ImGui::GetBackgroundDrawList();
        background->AddRectFilled(ImVec2(8, 8), ImVec2(40, 24), IM_COL32(255, 0, 0, 255));
        background->AddImage(ImTextureRef(blue), ImVec2(48, 8), ImVec2(80, 24));
        background->AddImage(green.GetTexRef(), ImVec2(48, 32), ImVec2(80, 56));
        background->PushClipRect(ImVec2(0, 48), ImVec2(32, 64), true);
        background->AddRectFilled(ImVec2(0, 40), ImVec2(64, 64), IM_COL32(255, 255, 0, 255));
        background->PopClipRect();
        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        beforePrepare();

        CommandList list = kit.list(QueueType::Graphics, "imgui.list");
        test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
        imgui::prepare(list, drawData);
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear, .store = StoreOp::Store } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kWidth, kHeight }, .label = "imgui.pass" });
        imgui::render(list, drawData);
        list.endRenderPass();
        test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
        kit.run(std::array{ list });
        kit.nextFrame();
    }

    //==================================================================================================================
    /// Reads `target` back and requires every pixel to equal the expectation with the managed texture's image in `managed`.
    void checkPixels(test::GpuKit& kit, Texture target, Texel managed)
    {
        const std::vector<std::byte> bytes = kit.readTexture(target, TextureRegion{ .extent = { kWidth, kHeight, 1 } }, Format::RGBA8Unorm);
        const std::array rects{ Rect{ 8, 8, 40, 24, { 255, 0, 0, 255 } },
                                Rect{ 48, 8, 80, 24, { 0, 0, 255, 255 } },
                                Rect{ 48, 32, 80, 56, managed },
                                Rect{ 0, 48, 32, 64, { 255, 255, 0, 255 } } };
        const std::vector<Texel> want = expectation(rects);
        REQUIRE(bytes.size() == want.size() * sizeof(Texel));
        std::vector<Texel> got(want.size());
        std::memcpy(got.data(), bytes.data(), bytes.size());
        uint32_t mismatches = 0;
        for (uint32_t i = 0; i < got.size(); ++i)
        {
            if (std::memcmp(&got[i], &want[i], sizeof(Texel)) == 0)
            {
                continue;
            }
            if (++mismatches <= 5)
            {
                std::printf(
                    "(%u, %u) got %u,%u,%u,%u want %u,%u,%u,%u\n",
                    i % kWidth,
                    i / kWidth,
                    got[i].r,
                    got[i].g,
                    got[i].b,
                    got[i].a,
                    want[i].r,
                    want[i].g,
                    want[i].b,
                    want[i].a
                );
            }
        }
        if (mismatches != 0)
        {
            FAIL(std::to_string(mismatches) + " pixels differ");
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("the adapter draws a fixed draw list exactly", "[imgui]")
{
    test::requireGpu();
    test::GpuKit kit(2);
    const Texture target = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm,
                     .extent = { kWidth, kHeight, 1 },
                     .usage = TextureUsage::RenderTarget | TextureUsage::Sampled,
                     .label = "imgui.pixels" }
    );
    const std::array<Texel, 4> blueTexels{ Texel{ 0, 0, 255, 255 }, Texel{ 0, 0, 255, 255 }, Texel{ 0, 0, 255, 255 }, Texel{ 0, 0, 255, 255 } };
    const std::array initial{ TextureData{ std::as_bytes(std::span(blueTexels)), 2 * sizeof(Texel), 0 } };
    const Texture blue = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 2, 2, 1 }, .usage = TextureUsage::Sampled, .label = "imgui.blue" }, initial
    );
    const ViewIndex blueView = *kit.device.createView(blue, {});

    ImGuiContext* context = ImGui::CreateContext();
    ImTextureData green;
    // Leaves the context and the adapter clean when a check below ends the case early, including a failed init.
    struct Teardown
    {
        ImTextureData& green;
        ImGuiContext* context;
        test::GpuKit& kit;
        Texture target;
        Texture blue;
        ViewIndex blueView;
        bool initialized = false;
        bool registered = false;

        ~Teardown()
        {
            if (registered)
            {
                ImGui::UnregisterUserTexture(&green);
            }
            if (initialized)
            {
                imgui::shutdown();
            }
            ImGui::DestroyContext(context);
            kit.device.destroy(blueView);
            kit.device.destroy(target);
            kit.device.destroy(blue);
        }
    } teardown{ green, context, kit, target, blue, blueView };

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(kWidth), float(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    REQUIRE(imgui::init(imgui::InitDesc{ .device = kit.device, .colorFormat = Format::RGBA8Unorm }).has_value());
    teardown.initialized = true;
    green.Create(ImTextureFormat_RGBA32, 4, 4);
    fill(green, Texel{ 0, 255, 0, 255 });
    ImGui::RegisterUserTexture(&green);
    teardown.registered = true;

    drawFrame(kit, target, imgui::textureId(blueView), green, [] {});
    checkPixels(kit, target, Texel{ 0, 255, 0, 255 });
    // The second frame rewrites the whole texture through the staging copy.
    drawFrame(kit, target, imgui::textureId(blueView), green, [&] {
        fill(green, Texel{ 255, 0, 255, 255 });
        const ImTextureRect whole{ 0, 0, 4, 4 };
        green.Updates.push_back(whole);
        green.UpdateRect = whole;
        green.SetStatus(ImTextureStatus_WantUpdates);
    });

    checkPixels(kit, target, Texel{ 255, 0, 255, 255 });
}
