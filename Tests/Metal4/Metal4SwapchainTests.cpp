//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4SwapchainTests.cpp
/// @brief Tests Metal 4 swapchains against their layer, the drawables they acquire and present, and the graphics queue's calls.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Core/Swapchains.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Formats.h"
#include "Metal4Presentation.h"
#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <CoreGraphics/CoreGraphics.h>
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <objc/message.h>
#include <objc/runtime.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// A new layer of `width` by `height` drawable pixels, retained; the caller releases it with `releaseLayer`. Each case
    /// takes layers of its own, so a resize leaves the shared test surfaces alone.
    CA::MetalLayer* newLayer(double width, double height)
    {
        const metal4::AutoreleasePool pool;
        CA::MetalLayer* layer = CA::MetalLayer::layer()->retain();
        layer->setDrawableSize(CGSize{ width, height });
        return layer;
    }

    //==================================================================================================================
    /// Releases a layer `newLayer` returned.
    void releaseLayer(CA::MetalLayer* layer)
    {
        const metal4::AutoreleasePool pool;
        layer->release();
    }

    //==================================================================================================================
    /// The residency set `layer` vends, read inside a pool because the getter may autorelease.
    MTL::ResidencySet* residencySetOf(CA::MetalLayer* layer)
    {
        const metal4::AutoreleasePool pool;
        return layer->residencySet();
    }

    //==================================================================================================================
    /// True when the color space of `layer` is the one named `name`.
    bool hasColorSpace(CA::MetalLayer* layer, CFStringRef name)
    {
        const metal4::AutoreleasePool pool;
        CGColorSpaceRef space = layer->colorspace();
        if (space == nullptr)
        {
            return false;
        }
        CFStringRef actual = CGColorSpaceCopyName(space);
        const bool same = actual != nullptr && CFEqual(actual, name);
        if (actual != nullptr)
        {
            CFRelease(actual);
        }
        return same;
    }

    //==================================================================================================================
    /// Reads the layer's `wantsExtendedDynamicRangeContent`, which metal-cpp does not wrap.
    bool wantsExtendedDynamicRange(CA::MetalLayer* layer)
    {
        static const SEL selector = sel_registerName("wantsExtendedDynamicRangeContent");
        using Getter = BOOL (*)(const void*, SEL);
        return reinterpret_cast<Getter>(&objc_msgSend)(layer, selector) != NO;
    }

    //==================================================================================================================
    /// The number of native queue calls `queue` makes for a submission of no list, measured on `device`.
    uint64_t submitCalls(Device device, Queue queue)
    {
        const uint64_t before = device.counters().nativeSubmitCalls;
        queue.submit({});
        return device.counters().nativeSubmitCalls - before;
    }

    //==================================================================================================================
    /// Opens and ends `count` frames with no work.
    void runFrames(Device device, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            device.beginFrame();
            device.endFrame();
        }
    }

    //==================================================================================================================
    /// The native texture the texture handle `image` of `device` carries.
    MTL::Texture* nativeTexture(Device device, Texture image)
    {
        return static_cast<MTL::Texture*>(core::coreOf(device).pool<Texture>().native(image));
    }

    //==================================================================================================================
    /// The texture of the drawable `swapchain` holds, read inside a pool because the getter may autorelease.
    MTL::Texture* drawableTexture(Swapchain swapchain)
    {
        const metal4::AutoreleasePool pool;
        CA::MetalDrawable* drawable = SwapchainImpl::of(swapchain).native.drawable;
        return drawable != nullptr ? drawable->texture() : nullptr;
    }

    //==================================================================================================================
    /// Waits up to two seconds for the pending presents of `swapchain` to reach zero and returns how many are left.
    uint32_t settledPresents(Swapchain swapchain)
    {
        const metal4::PresentLatency& latency = *SwapchainImpl::of(swapchain).native.latency;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (latency.count.load() != 0 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return latency.count.load();
    }

    /// One color space with the format chapter 11 lists for it, the layer color space it maps to and whether it turns EDR on.
    struct SpaceCase
    {
        ColorSpace colorSpace;
        Format format;
        CFStringRef name;
        bool extendedDynamicRange;
    };
} // namespace

//======================================================================================================================
TEST_CASE("each color space with every image count and present mode configures the layer", "[metal4]")
{
    Device device = *createDevice();
    CA::MetalLayer* layer = newLayer(256, 256);
    // An EDR space comes before SRGB, so SRGB is seen to turn EDR off again.
    const std::array<SpaceCase, 3> spaces{
        SpaceCase{ ColorSpace::ExtendedLinear, Format::RGBA16Float, kCGColorSpaceExtendedLinearSRGB, true },
        SpaceCase{ ColorSpace::SRGB, Format::BGRA8Unorm, kCGColorSpaceSRGB, false },
        SpaceCase{ ColorSpace::HDR10, Format::RGB10A2Unorm, kCGColorSpaceITUR_2100_PQ, true },
    };
    for (const SpaceCase& space : spaces)
    {
        for (const PresentMode mode : { PresentMode::Fifo, PresentMode::Mailbox, PresentMode::Immediate })
        {
            for (const uint32_t imageCount : { 2u, 3u })
            {
                const Swapchain swapchain = *device.createSwapchain(
                    SwapchainDesc{ .nativeSurface = layer,
                                   .format = space.format,
                                   .colorSpace = space.colorSpace,
                                   .imageCount = imageCount,
                                   .mode = mode,
                                   .maxLatency = 1 }
                );
                CHECK(layer->device() == DeviceImpl::of(device).device);
                CHECK(layer->pixelFormat() == metal4::pixelFormat(space.format));
                CHECK(hasColorSpace(layer, space.name));
                CHECK(wantsExtendedDynamicRange(layer) == space.extendedDynamicRange);
                CHECK(layer->framebufferOnly());
                CHECK(layer->maximumDrawableCount() == imageCount);
                CHECK(layer->displaySyncEnabled() == (mode == PresentMode::Fifo));
                device.destroy(swapchain);
            }
        }
    }
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("a swapchain takes its extent from the layer's drawable size", "[metal4]")
{
    Device device = *createDevice();
    CA::MetalLayer* sized = newLayer(320, 180);
    CA::MetalLayer* empty = newLayer(0, 0);
    const Counters before = device.counters();
    const Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = sized });
    CHECK(device.counters().nativeCreateCalls == before.nativeCreateCalls + 1);
    CHECK(device.counters().cpuAllocations == before.cpuAllocations + 1);
    CHECK(core::coreOf(swapchain).extent.width == 320);
    CHECK(core::coreOf(swapchain).extent.height == 180);
    // A layer with no drawable size, as one not yet in a window, gives a zero extent: the consumer resizes before presenting.
    const Swapchain unsized = *device.createSwapchain(SwapchainDesc{ .nativeSurface = empty });
    CHECK(core::coreOf(unsized).extent.width == 0);
    CHECK(core::coreOf(unsized).extent.height == 0);
    device.destroy(swapchain);
    device.destroy(unsized);
    destroyDevice(device);
    releaseLayer(empty);
    releaseLayer(sized);
}

//======================================================================================================================
TEST_CASE("resize sets the layer's drawable size after waiting for the last ended frame", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    CA::MetalLayer* layer = newLayer(256, 256);
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    // Before the first frame no work can name the images, so there is nothing to wait for.
    const Counters fresh = device.counters();
    swapchain.resize({ 640, 480 });
    CHECK(device.counters().nativeHostCalls == fresh.nativeHostCalls);
    CHECK(layer->drawableSize().width == 640);
    CHECK(layer->drawableSize().height == 480);
    CHECK(core::coreOf(swapchain).extent.width == 640);
    CHECK(core::coreOf(swapchain).extent.height == 480);
    runFrames(device, 1);
    const Counters framed = device.counters();
    swapchain.resize({ 800, 600 });
    CHECK(device.counters().nativeHostCalls == framed.nativeHostCalls + 1);
    CHECK(device.counters().nativeSubmitCalls == framed.nativeSubmitCalls);
    CHECK(layer->drawableSize().width == 800);
    CHECK(layer->drawableSize().height == 600);
    device.destroy(swapchain);
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("a swapchain's residency set is attached by the next graphics submission", "[metal4]")
{
    Device device = *createDevice();
    Queue graphics = device.queue(QueueType::Graphics);
    Queue compute = device.queue(QueueType::Compute);
    metal4::Presentation& presentation = DeviceImpl::of(device).presentation;
    CA::MetalLayer* layer = newLayer(256, 256);
    // The first submission commits the device's residency set; what follows measures a plain submission.
    graphics.submit({});
    const uint64_t plain = submitCalls(device, graphics);
    const Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    MTL::ResidencySet* set = residencySetOf(layer);
    REQUIRE(set != nullptr);
    CHECK(metal4::attachmentCount(presentation, set) == 0);
    // A compute submission applies nothing; the graphics queue alone holds a layer's set.
    CHECK(submitCalls(device, compute) == plain);
    CHECK(metal4::attachmentCount(presentation, set) == 0);
    CHECK(submitCalls(device, graphics) == plain + 1);
    CHECK(metal4::attachmentCount(presentation, set) == 1);
    CHECK(submitCalls(device, graphics) == plain);
    device.destroy(swapchain);
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("two swapchains on two layers attach two residency sets", "[metal4]")
{
    Device device = *createDevice();
    Queue graphics = device.queue(QueueType::Graphics);
    metal4::Presentation& presentation = DeviceImpl::of(device).presentation;
    CA::MetalLayer* first = newLayer(256, 256);
    CA::MetalLayer* second = newLayer(128, 128);
    graphics.submit({});
    const uint64_t plain = submitCalls(device, graphics);
    const Swapchain a = *device.createSwapchain(SwapchainDesc{ .nativeSurface = first });
    const Swapchain b = *device.createSwapchain(SwapchainDesc{ .nativeSurface = second });
    REQUIRE(residencySetOf(first) != residencySetOf(second));
    CHECK(submitCalls(device, graphics) == plain + 2);
    CHECK(metal4::attachmentCount(presentation, residencySetOf(first)) == 1);
    CHECK(metal4::attachmentCount(presentation, residencySetOf(second)) == 1);
    device.destroy(a);
    device.destroy(b);
    destroyDevice(device);
    releaseLayer(second);
    releaseLayer(first);
}

//======================================================================================================================
TEST_CASE("a new swapchain on a layer whose old swapchain has not retired keeps the set attached", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    Queue graphics = device.queue(QueueType::Graphics);
    metal4::Presentation& presentation = DeviceImpl::of(device).presentation;
    CA::MetalLayer* layer = newLayer(256, 256);
    graphics.submit({});
    const uint64_t plain = submitCalls(device, graphics);
    const Swapchain old = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    MTL::ResidencySet* set = residencySetOf(layer);
    REQUIRE(set != nullptr);
    CHECK(submitCalls(device, graphics) == plain + 1);
    device.destroy(old);
    const Swapchain current = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    // The set is already on the queue: the second attach only counts.
    CHECK(submitCalls(device, graphics) == plain);
    CHECK(metal4::attachmentCount(presentation, set) == 2);
    // The old swapchain retires and its detach leaves the set attached for the new one.
    runFrames(device, 3);
    CHECK(submitCalls(device, graphics) == plain);
    CHECK(metal4::attachmentCount(presentation, set) == 1);
    // The new one's retirement detaches the set from the queue.
    device.destroy(current);
    runFrames(device, 3);
    CHECK(submitCalls(device, graphics) == plain + 1);
    CHECK(metal4::attachmentCount(presentation, set) == 0);
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("a destroyed swapchain retires framesInFlight frames later", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    CA::MetalLayer* layer = newLayer(256, 256);
    const Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    device.beginFrame();
    device.destroy(swapchain);
    device.endFrame();
    const Counters destroyed = device.counters();
    runFrames(device, 1);
    CHECK(device.counters().deferredDestructionsRetired == destroyed.deferredDestructionsRetired);
    device.beginFrame();
    // The swapchain is a deferred destruction and a native release, as on the Null backend.
    CHECK(device.counters().deferredDestructionsRetired == destroyed.deferredDestructionsRetired + 1);
    CHECK(device.counters().nativeCreateCalls == destroyed.nativeCreateCalls + 1);
    device.endFrame();
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("destroyDevice frees a live swapchain and one not yet retired", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    CA::MetalLayer* live = newLayer(256, 256);
    CA::MetalLayer* retiring = newLayer(256, 256);
    (void)*device.createSwapchain(SwapchainDesc{ .nativeSurface = live });
    const Swapchain destroyed = *device.createSwapchain(SwapchainDesc{ .nativeSurface = retiring });
    device.queue(QueueType::Graphics).submit({});
    device.destroy(destroyed);
    // The live swapchain and the retiring one are freed and their sets detached after the queues drain.
    destroyDevice(device);
    releaseLayer(retiring);
    releaseLayer(live);
}

//======================================================================================================================
TEST_CASE("an acquired image carries the drawable's texture at the swapchain's extent and format", "[metal4]")
{
    Device device = *createDevice();
    CA::MetalLayer* layer = newLayer(320, 180);
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    device.beginFrame();
    const Counters before = device.counters();
    const Texture image = swapchain.acquire();
    REQUIRE(image != Texture{});
    // One nextDrawable is one native host call; the image is a memory object the backend did not create.
    CHECK(device.counters().nativeHostCalls == before.nativeHostCalls + 1);
    CHECK(device.counters().gpuAllocations == before.gpuAllocations + 1);
    CHECK(device.counters().nativeSubmitCalls == before.nativeSubmitCalls);
    MTL::Texture* texture = nativeTexture(device, image);
    REQUIRE(texture != nullptr);
    CHECK(texture == drawableTexture(swapchain));
    CHECK(texture->width() == 320);
    CHECK(texture->height() == 180);
    CHECK(texture->pixelFormat() == metal4::pixelFormat(Format::BGRA8Unorm));
    device.queue(QueueType::Graphics).present(swapchain);
    device.endFrame();
    device.destroy(swapchain);
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("the next graphics submission after an acquire waits for the drawable, and present signals and presents it", "[metal4]")
{
    Device device = *createDevice();
    Queue graphics = device.queue(QueueType::Graphics);
    Queue compute = device.queue(QueueType::Compute);
    CA::MetalLayer* layer = newLayer(256, 256);
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    // The first graphics submission attaches the layer's set and commits the device's; what follows measures plain ones.
    graphics.submit({});
    const uint64_t plain = submitCalls(device, graphics);
    const uint64_t plainCompute = submitCalls(device, compute);
    device.beginFrame();
    const Texture image = swapchain.acquire();
    REQUIRE(image != Texture{});
    CHECK(submitCalls(device, compute) == plainCompute);
    CHECK(submitCalls(device, graphics) == plain + 1);
    CHECK(submitCalls(device, graphics) == plain);
    const Counters before = device.counters();
    graphics.present(swapchain);
    // signalDrawable is a queue call and present a host call; the wait already went with the submission.
    CHECK(device.counters().nativeSubmitCalls == before.nativeSubmitCalls + 1);
    CHECK(device.counters().nativeHostCalls == before.nativeHostCalls + 1);
    CHECK(core::coreOf(device).pool<Texture>().lookup(image).status == core::LookupStatus::StaleGeneration);
    CHECK(SwapchainImpl::of(swapchain).native.drawable == nullptr);
    device.endFrame();
    device.destroy(swapchain);
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("present with no submission since the acquire waits for the drawable first", "[metal4]")
{
    Device device = *createDevice();
    Queue graphics = device.queue(QueueType::Graphics);
    CA::MetalLayer* layer = newLayer(256, 256);
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    graphics.submit({});
    const uint64_t plain = submitCalls(device, graphics);
    device.beginFrame();
    REQUIRE(swapchain.acquire() != Texture{});
    const Counters before = device.counters();
    graphics.present(swapchain);
    CHECK(device.counters().nativeSubmitCalls == before.nativeSubmitCalls + 2);
    CHECK(device.counters().nativeHostCalls == before.nativeHostCalls + 1);
    // The wait was issued by present, so the next submission has nothing to wait for.
    CHECK(submitCalls(device, graphics) == plain);
    device.endFrame();
    device.destroy(swapchain);
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("acquire returns null after about one second while maxLatency presents are pending, and changes nothing", "[metal4]")
{
    Device device = *createDevice();
    Queue graphics = device.queue(QueueType::Graphics);
    CA::MetalLayer* layer = newLayer(256, 256);
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer, .imageCount = 2, .maxLatency = 1 });
    graphics.submit({});
    metal4::PresentLatency& latency = *SwapchainImpl::of(swapchain).native.latency;
    const core::SwapchainCore& state = core::coreOf(swapchain);
    device.beginFrame();
    latency.count.store(1);
    const Counters before = device.counters();
    const auto start = std::chrono::steady_clock::now();
    const Texture none = swapchain.acquire();
    const std::chrono::duration<double> waited = std::chrono::steady_clock::now() - start;
    CHECK(none == Texture{});
    CHECK(waited.count() >= 0.9);
    CHECK(waited.count() < 3.5);
    CHECK(state.acquired == core::SwapchainCore::kNoImage);
    CHECK(state.next == 0);
    CHECK(SwapchainImpl::of(swapchain).native.drawable == nullptr);
    CHECK(device.counters().gpuAllocations == before.gpuAllocations);
    CHECK(device.counters().nativeHostCalls == before.nativeHostCalls);
    // With the limit no longer reached, a present raises the count and its presented handler brings it back.
    latency.count.store(0);
    REQUIRE(swapchain.acquire() != Texture{});
    graphics.present(swapchain);
    CHECK(settledPresents(swapchain) == 0);
    device.endFrame();
    device.destroy(swapchain);
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("after resize the next acquired image has the new extent", "[metal4]")
{
    Device device = *createDevice();
    Queue graphics = device.queue(QueueType::Graphics);
    CA::MetalLayer* layer = newLayer(256, 256);
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    device.beginFrame();
    REQUIRE(swapchain.acquire() != Texture{});
    graphics.present(swapchain);
    device.endFrame();
    swapchain.resize({ 128, 64 });
    device.beginFrame();
    const Texture image = swapchain.acquire();
    REQUIRE(image != Texture{});
    CHECK(nativeTexture(device, image)->width() == 128);
    CHECK(nativeTexture(device, image)->height() == 64);
    graphics.present(swapchain);
    device.endFrame();
    device.destroy(swapchain);
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("a swapchain of zero extent acquires null without asking the layer", "[metal4]")
{
    Device device = *createDevice();
    CA::MetalLayer* layer = newLayer(0, 0);
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    device.beginFrame();
    const Counters before = device.counters();
    CHECK(swapchain.acquire() == Texture{});
    CHECK(device.counters().nativeHostCalls == before.nativeHostCalls);
    CHECK(core::coreOf(swapchain).acquired == core::SwapchainCore::kNoImage);
    device.endFrame();
    device.destroy(swapchain);
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("a swapchain destroyed with an image acquired hands its drawable back to the layer when it retires", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    CA::MetalLayer* layer = newLayer(256, 256);
    // Two swapchains each keep a drawable of a layer that has two; held, they would leave none for the third.
    for (uint32_t i = 0; i < 2; ++i)
    {
        Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer, .imageCount = 2 });
        device.beginFrame();
        REQUIRE(swapchain.acquire() != Texture{});
        device.destroy(swapchain);
        device.endFrame();
    }
    runFrames(device, 3);
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer, .imageCount = 2 });
    for (uint32_t frame = 0; frame < 4; ++frame)
    {
        device.beginFrame();
        const auto start = std::chrono::steady_clock::now();
        const Texture image = swapchain.acquire();
        const std::chrono::duration<double> waited = std::chrono::steady_clock::now() - start;
        REQUIRE(image != Texture{});
        CHECK(waited.count() < 0.9);
        device.queue(QueueType::Graphics).present(swapchain);
        device.endFrame();
    }
    device.destroy(swapchain);
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("a swapchain destroyed with an image acquired leaves no drawable wait for the next graphics submission", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    Queue graphics = device.queue(QueueType::Graphics);
    CA::MetalLayer* layer = newLayer(256, 256);
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    graphics.submit({});
    const uint64_t plain = submitCalls(device, graphics);
    device.beginFrame();
    REQUIRE(swapchain.acquire() != Texture{});
    device.destroy(swapchain);
    device.endFrame();
    // The drawable will never be written or presented, so no queue waits for it; its swapchain releases it at retirement.
    device.beginFrame();
    CHECK(submitCalls(device, graphics) == plain);
    device.endFrame();
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("destroyDevice releases a drawable a live swapchain still holds", "[metal4]")
{
    Device device = *createDevice();
    CA::MetalLayer* layer = newLayer(256, 256);
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
    device.beginFrame();
    REQUIRE(swapchain.acquire() != Texture{});
    device.endFrame();
    // The swapchain is left live with its image acquired: destroyDevice retires the image and frees the swapchain.
    destroyDevice(device);
    releaseLayer(layer);
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0002 an image handle is stale after present", "[metal4][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0002", [] {
        Device device = *createDevice();
        CA::MetalLayer* layer = newLayer(256, 256);
        Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = layer });
        device.beginFrame();
        const Texture image = swapchain.acquire();
        device.queue(QueueType::Graphics).present(swapchain);
        device.destroy(image);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0002 a swapchain is created on an object that is not a Metal layer", "[metal4][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0002", [] {
        Device device = *createDevice();
        const metal4::AutoreleasePool pool;
        NS::String* text = metal4::makeString("not a layer");
        (void)device.createSwapchain(SwapchainDesc{ .nativeSurface = text });
    });
}
