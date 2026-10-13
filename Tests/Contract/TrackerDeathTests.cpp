//----------------------------------------------------------------------------------------------------------------------
/// @file TrackerDeathTests.cpp
/// @brief Death tests for the texture barrier rules the validation tracker judges at submission.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// A stand-in for a native surface: the Null backend never dereferences it.
    int g_surface = 0;

    //==================================================================================================================
    /// Records the barrier that moves every subresource of `texture` from `oldLayout` to `newLayout`, with no access on either
    /// side, so only the layout rules can fail.
    void move(CommandList list, Texture texture, Layout oldLayout, Layout newLayout)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{
            .texture = texture, .srcStage = Stage::All, .dstStage = Stage::All, .oldLayout = oldLayout, .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0013 a texture barrier starts from a layout the texture is not in", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0013", [] {
        PassKit kit(QueueType::Graphics, true);
        // A texture created without initial data is in Undefined.
        move(kit.list, kit.texture(), Layout::ShaderRead, Layout::CopyDst);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0014 the first barrier on an acquired image starts from a layout other than Undefined", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0014", [] {
        Device device = *createDevice(DeviceDesc{ .validation = true });
        Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surface, .label = "test.swapchain" });
        device.beginFrame();
        const Texture image = swapchain.acquire();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        move(list, image, Layout::Present, Layout::ColorAttachment);
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(QueueType::Graphics).submit(lists);
    });
}
