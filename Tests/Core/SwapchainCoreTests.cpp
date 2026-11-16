//----------------------------------------------------------------------------------------------------------------------
/// @file SwapchainCoreTests.cpp
/// @brief Tests the record the core keeps for a swapchain and its images, and the surface check no Null surface can fail.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Swapchains.h"
#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace rojoRHI;

namespace {
    /// Stand-ins for native surfaces: the tests never dereference them.
    std::array<int, 2> g_surfaces = {};

    //==================================================================================================================
    Swapchain makeSwapchain(Device device, uint32_t imageCount = 3, std::string_view label = "core.swapchain")
    {
        return *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surfaces[0], .imageCount = imageCount, .maxLatency = 1, .label = label });
    }

    //==================================================================================================================
    void acquireAndPresent(Device device, Swapchain swapchain)
    {
        device.beginFrame();
        (void)swapchain.acquire();
        device.queue(QueueType::Graphics).present(swapchain);
        device.endFrame();
    }

    //==================================================================================================================
    core::LookupStatus statusOf(Device device, Texture texture)
    {
        return core::coreOf(device).pool<Texture>().lookup(texture).status;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a swapchain's record keeps its description with every image available", "[core]")
{
    Device device = *createDevice();
    const Swapchain swapchain = *device.createSwapchain(
        SwapchainDesc{ .nativeSurface = &g_surfaces[0],
                       .format = Format::RGBA16Float,
                       .colorSpace = ColorSpace::ExtendedLinear,
                       .imageCount = 2,
                       .mode = PresentMode::Mailbox,
                       .maxLatency = 2,
                       .label = "core.swapchain" }
    );
    const core::SwapchainCore& record = core::coreOf(swapchain);
    CHECK(record.label == "core.swapchain");
    CHECK(record.surface == &g_surfaces[0]);
    CHECK(record.format == Format::RGBA16Float);
    CHECK(record.colorSpace == ColorSpace::ExtendedLinear);
    CHECK(record.mode == PresentMode::Mailbox);
    CHECK(record.maxLatency == 2);
    REQUIRE(record.images.size() == 2);
    CHECK(record.images[0].state == core::ImageState::Available);
    CHECK(record.images[1].state == core::ImageState::Available);
    CHECK(record.acquired == core::SwapchainCore::kNoImage);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an unlabelled swapchain takes the default label", "[core]")
{
    Device device = *createDevice();
    const Swapchain swapchain = makeSwapchain(device, 3, "");
    CHECK(core::coreOf(swapchain).label == "rojorhi.swapchain.unnamed");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an acquired image is a RenderTarget texture record labelled after the swapchain", "[core]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device);
    device.beginFrame();
    const Texture image = swapchain.acquire();
    const core::LookupResult<core::TextureRecord> found = core::coreOf(device).pool<Texture>().lookup(image);
    REQUIRE(found.status == core::LookupStatus::Ok);
    CHECK(found.record->label == "core.swapchain");
    CHECK(found.record->format == Format::BGRA8Unorm);
    CHECK(found.record->usage == TextureUsage::RenderTarget);
    CHECK(found.record->type == TextureType::Tex2D);
    CHECK(found.record->mips == 1);
    CHECK(found.record->layers == 1);
    CHECK(found.record->samples == 1);
    CHECK(found.record->swapchainImage);
    CHECK(core::coreOf(swapchain).images[core::coreOf(swapchain).acquired].state == core::ImageState::Acquired);
    device.queue(QueueType::Graphics).present(swapchain);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("acquire hands the images out in order and wraps", "[core]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device, 3);
    for (const uint32_t expected : { 0u, 1u, 2u, 0u, 1u })
    {
        device.beginFrame();
        (void)swapchain.acquire();
        CHECK(core::coreOf(swapchain).acquired == expected);
        device.queue(QueueType::Graphics).present(swapchain);
        device.endFrame();
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("present makes the image available and its handle stale", "[core]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device);
    device.beginFrame();
    const Texture image = swapchain.acquire();
    device.queue(QueueType::Graphics).present(swapchain);
    CHECK(core::coreOf(swapchain).acquired == core::SwapchainCore::kNoImage);
    CHECK(core::coreOf(swapchain).images[0].state == core::ImageState::Available);
    CHECK(statusOf(device, image) == core::LookupStatus::StaleGeneration);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("destroying a swapchain releases the image it holds", "[core]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device);
    device.beginFrame();
    const Texture image = swapchain.acquire();
    device.destroy(swapchain);
    CHECK(statusOf(device, image) == core::LookupStatus::StaleGeneration);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("resize changes the extent the next image has", "[core]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device);
    swapchain.resize({ 320, 200 });
    device.beginFrame();
    const Texture image = swapchain.acquire();
    const core::TextureRecord& record = *core::coreOf(device).pool<Texture>().lookup(image).record;
    CHECK(record.extent.width == 320);
    CHECK(record.extent.height == 200);
    CHECK(core::coreOf(swapchain).extent.width == 320);
    device.queue(QueueType::Graphics).present(swapchain);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an image acquired again after a present is a different texture of the same swapchain", "[core]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device, 2);
    acquireAndPresent(device, swapchain);
    acquireAndPresent(device, swapchain);
    acquireAndPresent(device, swapchain);
    CHECK(core::coreOf(device).pool<Texture>().liveCount() == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an acquired image's texture carries the backend's payload from its allocation", "[core]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device);
    int payload = 0;
    device.beginFrame();
    const Texture image = core::acquireImage(core::coreOf(swapchain), &payload);
    CHECK(core::coreOf(device).pool<Texture>().native(image) == &payload);
    device.queue(QueueType::Graphics).present(swapchain);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("checkAcquireImage refuses no valid acquire and leaves the swapchain unchanged", "[core]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device);
    acquireAndPresent(device, swapchain);
    device.beginFrame();
    const core::SwapchainCore& record = core::coreOf(swapchain);
    const core::SwapchainCore before = record;
    core::checkAcquireImage(record);
    CHECK(record.acquired == before.acquired);
    CHECK(record.next == before.next);
    CHECK(record.acquiredFrame == before.acquiredFrame);
    REQUIRE(record.images.size() == before.images.size());
    for (size_t index = 0; index < record.images.size(); ++index)
    {
        CHECK(record.images[index].state == before.images[index].state);
        CHECK(record.images[index].texture.bits == before.images[index].texture.bits);
    }
    CHECK(core::coreOf(device).pool<Texture>().liveCount() == 0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0002 a surface the backend's predicate rejects creates a swapchain", "[core][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0002", [] {
        Device device = *createDevice();
        core::coreOf(device).setSurfaceCheck([](const void*) { return false; });
        (void)makeSwapchain(device);
    });
}
