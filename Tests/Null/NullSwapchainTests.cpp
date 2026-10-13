//----------------------------------------------------------------------------------------------------------------------
/// @file NullSwapchainTests.cpp
/// @brief Tests what only the Null backend promises about swapchains: the surface is ignored and presents complete during the call.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Swapchains.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;

namespace {
    /// Stand-ins for native surfaces: the Null backend never dereferences them.
    std::array<int, 2> g_surfaces = {};
} // namespace

//======================================================================================================================
TEST_CASE("a Null swapchain's images have the same extent whatever the surface", "[null]")
{
    Device device = *createDevice();
    Swapchain a = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surfaces[0] });
    Swapchain b = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surfaces[1] });
    device.beginFrame();
    const Texture imageA = a.acquire();
    const Texture imageB = b.acquire();
    auto& textures = core::coreOf(device).pool<Texture>();
    CHECK(textures.lookup(imageA).record->extent.width == 1280);
    CHECK(textures.lookup(imageB).record->extent.height == 720);
    device.queue(QueueType::Graphics).present(a);
    device.queue(QueueType::Graphics).present(b);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null swapchain presents without waiting for a display", "[null]")
{
    Device device = *createDevice();
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surfaces[0], .imageCount = 2, .maxLatency = 1 });
    for (int frame = 0; frame < 5; ++frame)
    {
        device.beginFrame();
        CHECK(swapchain.acquire().bits != 0);
        device.queue(QueueType::Graphics).present(swapchain);
        device.endFrame();
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Release build creates a swapchain on a null surface", "[null]")
{
#ifdef NDEBUG
    Device device = *createDevice();
    CHECK(device.createSwapchain(SwapchainDesc{}).has_value());
    destroyDevice(device);
#else
    SKIP("VU-rojo-present-0001 is checked in a Debug build");
#endif
}
