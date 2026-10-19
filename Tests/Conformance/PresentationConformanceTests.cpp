//----------------------------------------------------------------------------------------------------------------------
/// @file PresentationConformanceTests.cpp
/// @brief Observes the acquire and present loop of chapter 11 on a GPU, through a surface outside any window.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <cstdint>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// Runs `frames` frames on `device`, each acquiring an image of `swapchain`, submitting an empty graphics submission when
    /// `submit` is set, and presenting it. Every acquire returns an image.
    void presentFrames(Device device, Swapchain swapchain, uint32_t frames, bool submit)
    {
        Queue graphics = device.queue(QueueType::Graphics);
        for (uint32_t frame = 1; frame <= frames; ++frame)
        {
            CAPTURE(frame);
            device.beginFrame();
            const Texture image = swapchain.acquire();
            REQUIRE(image != Texture{});
            if (submit)
            {
                graphics.submit({});
            }
            graphics.present(swapchain);
            device.endFrame();
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("every acquire returns an image within its deadline, for each image count, latency and framesInFlight", "[conformance]")
{
    test::requireGpu();
    const uint32_t imageCount = GENERATE(2u, 3u);
    const uint32_t maxLatency = GENERATE_COPY(range(1u, imageCount + 1));
    const uint32_t framesInFlight = GENERATE(2u, 3u);
    // Chapter 11: the graphics queue's submissions after an acquire are ordered after the display releases the image,
    // whether or not one comes between the acquire and the present.
    const bool submit = GENERATE(false, true);
    CAPTURE(imageCount, maxLatency, framesInFlight, submit);
    Device device = *createDevice({ .framesInFlight = framesInFlight });
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(0), .imageCount = imageCount, .maxLatency = maxLatency });
    presentFrames(device, swapchain, 3 * framesInFlight, submit);
    device.destroy(swapchain);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a swapchain resized between frames goes on acquiring and presenting", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .framesInFlight = 2 });
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(1) });
    presentFrames(device, swapchain, 6, true);
    swapchain.resize({ 128, 96 });
    presentFrames(device, swapchain, 6, true);
    // The test surfaces are shared by the process, so the surface gets back the extent it started with.
    swapchain.resize({ 256, 256 });
    presentFrames(device, swapchain, 2, true);
    device.destroy(swapchain);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two swapchains acquire and present in the same frames", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .framesInFlight = 2 });
    Queue graphics = device.queue(QueueType::Graphics);
    Swapchain first = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(2) });
    Swapchain second = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(3), .imageCount = 2, .maxLatency = 1 });
    for (uint32_t frame = 1; frame <= 6; ++frame)
    {
        CAPTURE(frame);
        device.beginFrame();
        const Texture a = first.acquire();
        const Texture b = second.acquire();
        REQUIRE(a != Texture{});
        REQUIRE(b != Texture{});
        graphics.submit({});
        graphics.present(first);
        graphics.present(second);
        device.endFrame();
    }
    device.destroy(first);
    device.destroy(second);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an image cleared by a pass and moved to Present by a barrier presents in every frame", "[conformance]")
{
    test::requireGpu();
    // Outside Release the validation tracker checks the frame's barriers, among them VU-rojo-present-0018 (the last barrier
    // before the present moves the image to Present), so a missing barrier fails the case; a Release build, which refuses
    // validation, runs it as a smoke case.
#ifdef NDEBUG
    constexpr bool kValidation = false;
#else
    constexpr bool kValidation = true;
#endif
    Device device = *createDevice({ .validation = kValidation, .framesInFlight = 2 });
    Queue graphics = device.queue(QueueType::Graphics);
    // The test surfaces start at 256 by 256, the extent a new swapchain takes (chapter 11).
    constexpr uint32_t kExtent = 256;
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(4) });
    for (uint32_t frame = 1; frame <= 6; ++frame)
    {
        CAPTURE(frame);
        device.beginFrame();
        const Texture image = swapchain.acquire();
        REQUIRE(image != Texture{});
        // Chapter 11's frame: from Undefined to ColorAttachment, render, then to Present before the present.
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.present");
        test::transition(list, image, Layout::Undefined, Layout::ColorAttachment);
        const std::array colors{ ColorAttachment{ .texture = image, .load = LoadOp::Clear } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kExtent, kExtent }, .label = "test.presentPass" });
        list.endRenderPass();
        test::transition(list, image, Layout::ColorAttachment, Layout::Present);
        list.end();
        graphics.submit(std::array{ list });
        graphics.present(swapchain);
        device.endFrame();
    }
    device.destroy(swapchain);
    destroyDevice(device);
}
