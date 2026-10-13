//----------------------------------------------------------------------------------------------------------------------
/// @file SwapchainDeathTests.cpp
/// @brief Death tests for the valid-usage rules of swapchains and presentation in chapter 11.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace rojoRHI;

namespace {
    /// Stand-ins for native surfaces: the tests never dereference them.
    std::array<int, 2> g_surfaces = {};

    //==================================================================================================================
    SwapchainDesc described(size_t surface = 0)
    {
        return SwapchainDesc{ .nativeSurface = &g_surfaces[surface], .label = "test.swapchain" };
    }

    //==================================================================================================================
    Swapchain makeSwapchain(Device device, size_t surface = 0)
    {
        return *device.createSwapchain(described(surface));
    }

    //==================================================================================================================
    TextureBarrier barrierOf(Texture texture, Layout oldLayout, Layout newLayout)
    {
        return TextureBarrier{ .texture = texture,
                               .range = { .mipCount = kAllMips },
                               .srcStage = Stage::All,
                               .dstStage = Stage::All,
                               .oldLayout = oldLayout,
                               .newLayout = newLayout };
    }

    /// An open frame, an acquired image and a graphics list, for the tests of the entries that refuse the image.
    struct Acquired
    {
        Device device;
        Swapchain swapchain;
        Texture image;
        CommandList list;
        Buffer buffer;
        Texture texture;
    };

    //==================================================================================================================
    /// Opens a frame and acquires the image, the list, a buffer and an ordinary texture.
    Acquired acquireKit()
    {
        Acquired kit;
        kit.device = *createDevice();
        kit.swapchain = makeSwapchain(kit.device);
        kit.device.beginFrame();
        kit.image = kit.swapchain.acquire();
        kit.list = kit.device.acquireCommandList(QueueType::Graphics, "test.list");
        kit.buffer = *kit.device.createBuffer(BufferDesc{ .size = 4096, .memory = Memory::Gpu, .usage = BufferUsage::None, .label = "test.buffer" });
        kit.texture = *kit.device.createTexture(
            TextureDesc{ .format = Format::BGRA8Unorm, .extent = { 8, 8, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.texture" }
        );
        return kit;
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-present-0001 a swapchain is created on a null surface", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0001", [] {
        Device device = *createDevice();
        SwapchainDesc desc = described();
        desc.nativeSurface = nullptr;
        (void)device.createSwapchain(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0003 a swapchain pairs a color space with another one's format", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0003", [] {
        Device device = *createDevice();
        SwapchainDesc desc = described();
        desc.colorSpace = ColorSpace::HDR10;
        (void)device.createSwapchain(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0004 a swapchain has one image", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0004", [] {
        Device device = *createDevice();
        SwapchainDesc desc = described();
        desc.imageCount = 1;
        desc.maxLatency = 1;
        (void)device.createSwapchain(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0004 a swapchain has four images", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0004", [] {
        Device device = *createDevice();
        SwapchainDesc desc = described();
        desc.imageCount = 4;
        (void)device.createSwapchain(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0005 a swapchain has a maximum latency of zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0005", [] {
        Device device = *createDevice();
        SwapchainDesc desc = described();
        desc.maxLatency = 0;
        (void)device.createSwapchain(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0005 a swapchain has a maximum latency above its image count", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0005", [] {
        Device device = *createDevice();
        SwapchainDesc desc = described();
        desc.imageCount = 2;
        desc.maxLatency = 3;
        (void)device.createSwapchain(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0006 a swapchain label is not valid UTF-8", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0006", [] {
        Device device = *createDevice();
        SwapchainDesc desc = described();
        desc.label = "\xC0\xAF";
        (void)device.createSwapchain(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0007 a swapchain label holds a NUL", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0007", [] {
        Device device = *createDevice();
        SwapchainDesc desc = described();
        desc.label = std::string_view("a\0b", 3);
        (void)device.createSwapchain(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0006 a surface of a live swapchain takes a second swapchain", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0006", [] {
        Device device = *createDevice();
        (void)makeSwapchain(device);
        (void)device.createSwapchain(described());
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0007 a swapchain is resized in an open frame", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0007", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        device.beginFrame();
        swapchain.resize({ 64, 64 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0008 a swapchain is resized with an image acquired", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0008", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        device.beginFrame();
        (void)swapchain.acquire();
        device.endFrame();
        swapchain.resize({ 64, 64 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0009 a swapchain is resized to a zero width", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0009", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        swapchain.resize({ 0, 64 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0009 a swapchain is resized to a zero height", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0009", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        swapchain.resize({ 64, 0 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0010 an image is acquired with no frame open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0010", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        (void)swapchain.acquire();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0011 a second image is acquired before the first is presented", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0011", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        device.beginFrame();
        (void)swapchain.acquire();
        (void)swapchain.acquire();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 an acquired image gets a view", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        device.beginFrame();
        const Texture image = swapchain.acquire();
        (void)device.createView(image, TextureViewDesc{ .usage = TextureUsage::Sampled });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 an acquired image is written into a view range", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        Acquired kit = acquireKit();
        const IndexRange range = *kit.device.reserveViews(1);
        kit.device.writeView(range, 0, kit.image, TextureViewDesc{ .usage = TextureUsage::Sampled });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 an acquired image is the destination of copyBufferToTexture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        Acquired kit = acquireKit();
        kit.list.copyBufferToTexture(kit.buffer, { .rowPitch = 256 }, kit.image, TextureRegion{ .extent = { 4, 1, 1 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 an acquired image is the source of copyTextureToBuffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        Acquired kit = acquireKit();
        kit.list.copyTextureToBuffer(kit.image, TextureRegion{ .extent = { 4, 1, 1 } }, kit.buffer, { .rowPitch = 256 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 an acquired image is the source of copyTexture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        Acquired kit = acquireKit();
        kit.list.copyTexture(kit.image, TextureRegion{ .extent = { 4, 4, 1 } }, kit.texture, TextureRegion{ .extent = { 4, 4, 1 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 an acquired image is the destination of copyTexture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        Acquired kit = acquireKit();
        kit.list.copyTexture(kit.texture, TextureRegion{ .extent = { 4, 4, 1 } }, kit.image, TextureRegion{ .extent = { 4, 4, 1 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 an acquired image is cleared by clearTexture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        Acquired kit = acquireKit();
        kit.list.clearTexture(kit.image, SubresourceRange{ .mipCount = kAllMips }, ClearValue{});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0013 an acquired image is destroyed", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0013", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        device.beginFrame();
        device.destroy(swapchain.acquire());
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0015 an image is presented on the compute queue", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0015", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        device.beginFrame();
        (void)swapchain.acquire();
        device.queue(QueueType::Compute).present(swapchain);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0016 an image is presented without an acquire", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0016", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        device.beginFrame();
        device.queue(QueueType::Graphics).present(swapchain);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0016 an image is presented twice", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0016", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        device.beginFrame();
        (void)swapchain.acquire();
        Queue queue = device.queue(QueueType::Graphics);
        queue.present(swapchain);
        queue.present(swapchain);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0017 an image is presented after the frame of its acquire ended", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0017", [] {
        Device device = *createDevice();
        Swapchain swapchain = makeSwapchain(device);
        device.beginFrame();
        (void)swapchain.acquire();
        device.endFrame();
        device.queue(QueueType::Graphics).present(swapchain);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0005 a barrier moves an ordinary texture to Present", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0005", [] {
        Device device = *createDevice();
        device.beginFrame();
        const Texture texture = *device.createTexture(
            TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.texture" }
        );
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        const std::array<TextureBarrier, 1> barriers{ barrierOf(texture, Layout::Undefined, Layout::Present) };
        list.barrier({}, {}, barriers);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0005 a barrier moves an ordinary texture from Present", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0005", [] {
        Device device = *createDevice();
        device.beginFrame();
        const Texture texture = *device.createTexture(
            TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.texture" }
        );
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        const std::array<TextureBarrier, 1> barriers{ barrierOf(texture, Layout::Present, Layout::ColorAttachment) };
        list.barrier({}, {}, barriers);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0004 a default-constructed Swapchain acquires", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0004", [] {
        Swapchain swapchain;
        (void)swapchain.acquire();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0005 a swapchain of another device is presented", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0005", [] {
        Device first = *createDevice();
        Device second = *createDevice();
        Swapchain swapchain = makeSwapchain(first);
        first.beginFrame();
        second.beginFrame();
        (void)swapchain.acquire();
        second.queue(QueueType::Graphics).present(swapchain);
    });
}
