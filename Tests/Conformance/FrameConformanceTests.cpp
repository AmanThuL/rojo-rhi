//----------------------------------------------------------------------------------------------------------------------
/// @file FrameConformanceTests.cpp
/// @brief Observes frame pacing and deferred destruction of chapter 02 on a GPU.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("beginFrame returns only after the graphics work of the frame framesInFlight earlier has completed, with framesInFlight 2 and 3", "[conformance]")
{
    test::requireGpu();
    const uint32_t framesInFlight = GENERATE(2u, 3u);
    CAPTURE(framesInFlight);
    Device device = *createDevice({ .framesInFlight = framesInFlight });
    // Chapter 10, "Frame pacing": beginFrame waits for the slot's fence from framesInFlight frames earlier, which the graphics
    // queue signals after the frame's graphics submissions, so a caller fence signalled there in frame N has reached N by then.
    const Fence fence = *device.createFence(0, "test.frame");
    Queue graphics = device.queue(QueueType::Graphics);
    for (uint32_t frame = 1; frame <= 3 * framesInFlight; ++frame)
    {
        CAPTURE(frame);
        device.beginFrame();
        if (frame > framesInFlight)
        {
            CHECK(device.completedValue(fence) >= frame - framesInFlight);
        }
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, frame } };
        graphics.submit({}, {}, signal);
        device.endFrame();
    }
    CHECK(device.wait({ fence, 3 * framesInFlight }, UINT64_MAX));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an object destroyed in frame N retires when frame N + framesInFlight begins and not before", "[conformance]")
{
    test::requireGpu();
    const uint32_t framesInFlight = GENERATE(2u, 3u);
    CAPTURE(framesInFlight);
    Device device = *createDevice({ .framesInFlight = framesInFlight });
    const Buffer buffer = *device.createBuffer({ .size = 64 });
    const Texture texture = *device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    const Texture host = *device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    const ViewIndex view = *device.createView(host, {});
    const SamplerIndex sampler = *device.createSampler({});

    device.beginFrame();
    const uint64_t before = device.counters().deferredDestructionsRetired;
    device.destroy(buffer);
    device.destroy(texture);
    device.destroy(view);
    device.destroy(sampler);
    device.endFrame();
    for (uint32_t later = 1; later < framesInFlight; ++later)
    {
        device.beginFrame();
        CAPTURE(later);
        CHECK(device.counters().deferredDestructionsRetired == before);
        // The freed view index is not handed out while its frame can still be in flight.
        const ViewIndex other = *device.createView(host, {});
        CHECK(other != view);
        device.destroy(other);
        device.endFrame();
    }
    device.beginFrame();
    CHECK(device.counters().deferredDestructionsRetired == before + 4);
    device.endFrame();
    destroyDevice(device);
}
