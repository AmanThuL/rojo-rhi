//----------------------------------------------------------------------------------------------------------------------
/// @file SwapchainTests.cpp
/// @brief Tests swapchains and presentation against chapter 11.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    /// Stand-ins for native surfaces: the tests never dereference them.
    std::array<int, 4> g_surfaces = {};

    //==================================================================================================================
    void* surface(size_t index)
    {
        return &g_surfaces[index];
    }

    std::vector<std::string> g_warnings;

    //==================================================================================================================
    [[maybe_unused]] void collectWarning(MessageSeverity severity, std::string_view message, void*)
    {
        if (severity == MessageSeverity::Warning)
        {
            g_warnings.emplace_back(message);
        }
    }

    //==================================================================================================================
    Swapchain makeSwapchain(Device device, size_t index = 0)
    {
        Result<Swapchain> created = device.createSwapchain(SwapchainDesc{ .nativeSurface = surface(index), .label = "test.swapchain" });
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Records the barrier that moves the whole image from `oldLayout` to `newLayout`.
    void moveImage(CommandList list, Texture image, Layout oldLayout, Layout newLayout)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = image,
                                                                      .range = { .mipCount = kAllMips },
                                                                      .srcStage = Stage::All,
                                                                      .dstStage = Stage::All,
                                                                      .oldLayout = oldLayout,
                                                                      .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Runs one frame of the order chapter 11 gives: acquire, render, present.
    void presentFrame(Device device, Swapchain swapchain)
    {
        device.beginFrame();
        const Texture image = swapchain.acquire();
        REQUIRE(image.bits != 0);
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        moveImage(list, image, Layout::Undefined, Layout::ColorAttachment);
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = image } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 1, 1 } });
        list.endRenderPass();
        moveImage(list, image, Layout::ColorAttachment, Layout::Present);
        list.end();
        Queue queue = device.queue(QueueType::Graphics);
        const std::array<CommandList, 1> lists{ list };
        queue.submit(lists);
        queue.present(swapchain);
        device.endFrame();
    }
} // namespace

//======================================================================================================================
TEST_CASE("a swapchain is created from a description with every default", "[contract]")
{
    Device device = *createDevice();
    CHECK(device.createSwapchain(SwapchainDesc{ .nativeSurface = surface(0) }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("each color space is created with its format", "[contract]")
{
    Device device = *createDevice();
    CHECK(device.createSwapchain(SwapchainDesc{ .nativeSurface = surface(0), .format = Format::BGRA8Unorm, .colorSpace = ColorSpace::SRGB }).has_value());
    CHECK(device.createSwapchain(SwapchainDesc{ .nativeSurface = surface(1), .format = Format::RGBA16Float, .colorSpace = ColorSpace::ExtendedLinear })
              .has_value());
    CHECK(device.createSwapchain(SwapchainDesc{ .nativeSurface = surface(2), .format = Format::RGB10A2Unorm, .colorSpace = ColorSpace::HDR10 }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("every image count and present mode is accepted", "[contract]")
{
    Device device = *createDevice();
    CHECK(device.createSwapchain(SwapchainDesc{ .nativeSurface = surface(0), .imageCount = 2, .mode = PresentMode::Mailbox, .maxLatency = 1 }).has_value());
    CHECK(device.createSwapchain(SwapchainDesc{ .nativeSurface = surface(1), .imageCount = 3, .mode = PresentMode::Immediate, .maxLatency = 3 }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("acquire returns a texture a frame can draw to and present", "[contract]")
{
    Device device = *createDevice();
    const Swapchain swapchain = makeSwapchain(device);
    presentFrame(device, swapchain);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a swapchain presents frame after frame", "[contract]")
{
    Device device = *createDevice();
    const Swapchain swapchain = makeSwapchain(device);
    for (int frame = 0; frame < 8; ++frame)
    {
        presentFrame(device, swapchain);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an acquired image is a new texture in each frame", "[contract]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device);
    device.beginFrame();
    const Texture first = swapchain.acquire();
    device.queue(QueueType::Graphics).present(swapchain);
    device.endFrame();
    device.beginFrame();
    const Texture second = swapchain.acquire();
    CHECK(first != second);
    device.queue(QueueType::Graphics).present(swapchain);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an image acquired and presented without work is accepted", "[contract]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device);
    device.beginFrame();
    CHECK(swapchain.acquire().bits != 0);
    device.queue(QueueType::Graphics).present(swapchain);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("resize outside a frame lets the next acquire and present proceed", "[contract]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device);
    presentFrame(device, swapchain);
    swapchain.resize({ 640, 480 });
    presentFrame(device, swapchain);
    swapchain.resize({ 1, 1 });
    presentFrame(device, swapchain);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a destroyed swapchain's surface takes a new swapchain", "[contract]")
{
    Device device = *createDevice();
    const Swapchain first = makeSwapchain(device);
    device.destroy(first);
    CHECK(device.createSwapchain(SwapchainDesc{ .nativeSurface = surface(0) }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two swapchains of one device present on their own surfaces", "[contract]")
{
    Device device = *createDevice();
    Swapchain a = makeSwapchain(device, 0);
    Swapchain b = makeSwapchain(device, 1);
    device.beginFrame();
    const Texture imageA = a.acquire();
    const Texture imageB = b.acquire();
    CHECK(imageA != imageB);
    Queue queue = device.queue(QueueType::Graphics);
    queue.present(a);
    queue.present(b);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a swapchain destroyed after many frames releases its slot", "[contract]")
{
    Device device = *createDevice();
    const Swapchain swapchain = makeSwapchain(device);
    for (int frame = 0; frame < 6; ++frame)
    {
        presentFrame(device, swapchain);
    }
    device.destroy(swapchain);
    for (int frame = 0; frame < 4; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a swapchain still live, with an image acquired, is drained by destroyDevice", "[contract]")
{
    Device device = *createDevice();
    Swapchain swapchain = makeSwapchain(device);
    device.beginFrame();
    CHECK(swapchain.acquire().bits != 0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("destroyDevice warns of a live swapchain once, not again for its acquired image", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    g_warnings.clear();
    setMessageCallback(collectWarning, nullptr);
    Device device = *createDevice(DeviceDesc{ .validation = true });
    Swapchain swapchain = makeSwapchain(device);
    device.beginFrame();
    CHECK(swapchain.acquire().bits != 0);
    device.endFrame();
    destroyDevice(device);
    setMessageCallback(nullptr, nullptr);
    CHECK(g_warnings.size() == 1);
#endif
}
