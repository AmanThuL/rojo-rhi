//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12SwapchainTests.cpp
/// @brief Tests D3D12 swapchains against the DXGI flip-model swapchains, back buffers and presents they make.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12Formats.h"
#include "D3D12Swapchain.h"
#include "D3D12Textures.h"
#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rojoRHI;

namespace {
    /// Guards `g_messages`, which the message callback may fill from any thread.
    std::mutex g_messagesMutex;

    /// The messages at `Warning` or above the library reported while a case collected them.
    std::vector<std::string> g_messages;

    //==================================================================================================================
    /// Keeps `message` when it is a warning or an error. It may run on any thread.
    void collectMessage(MessageSeverity severity, std::string_view message, void*)
    {
        if (severity == MessageSeverity::Info)
        {
            return;
        }
        const std::lock_guard lock(g_messagesMutex);
        g_messages.emplace_back(message);
    }

    /// Collects the library's warnings and errors, the debug layer's included, into `g_messages`, emptied first, while it
    /// lives, and removes the callback when it goes, however the case ends.
    class MessageCollection
    {
      public:
        MessageCollection();
        ~MessageCollection();

        //==============================================================================================================
        MessageCollection(const MessageCollection&) = delete;

        //==============================================================================================================
        MessageCollection& operator=(const MessageCollection&) = delete;

        /// Returns the messages collected so far, and lists them for the log.
        size_t count() const;
    };

    //==================================================================================================================
    MessageCollection::MessageCollection()
    {
        {
            const std::lock_guard lock(g_messagesMutex);
            g_messages.clear();
        }
        setMessageCallback(collectMessage, nullptr);
    }

    //==================================================================================================================
    MessageCollection::~MessageCollection()
    {
        setMessageCallback(nullptr, nullptr);
    }

    //==================================================================================================================
    size_t MessageCollection::count() const
    {
        const std::lock_guard lock(g_messagesMutex);
        for (const std::string& message : g_messages)
        {
            UNSCOPED_INFO("message: " << message);
        }
        return g_messages.size();
    }

    //==================================================================================================================
    /// Returns a new device with the default description, three frames in flight.
    Device makeDevice()
    {
        Result<Device> created = createDevice();
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new swapchain of `desc` on `device`.
    Swapchain makeSwapchain(Device device, const SwapchainDesc& desc)
    {
        Result<Swapchain> created = device.createSwapchain(desc);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns the native state of `swapchain`.
    d3d12::SwapchainNative& nativeOf(Swapchain swapchain)
    {
        return SwapchainImpl::of(swapchain).native;
    }

    //==================================================================================================================
    /// Returns the name `object` carries, without a terminating null.
    std::wstring objectName(ID3D12Object* object)
    {
        wchar_t name[128] = {};
        UINT size = static_cast<UINT>(sizeof(name));
        if (FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
        {
            return {};
        }
        // The size counts bytes; a terminating null, when stored, is dropped.
        std::wstring read(name, size / sizeof(wchar_t));
        while (!read.empty() && read.back() == L'\0')
        {
            read.pop_back();
        }
        return read;
    }

    //==================================================================================================================
    /// Checks that every image payload of `swapchain` holds the back buffer DXGI returns for its index, named
    /// `<label>.image<i>`. The buffers fetched here are released before the call returns.
    void checkBackBuffers(Swapchain swapchain, std::wstring_view label)
    {
        d3d12::SwapchainNative& native = nativeOf(swapchain);
        for (size_t i = 0; i < native.images.size(); ++i)
        {
            d3d12::Com<ID3D12Resource> buffer;
            REQUIRE(SUCCEEDED(native.swapchain->GetBuffer(static_cast<UINT>(i), IID_PPV_ARGS(buffer.put()))));
            CHECK(native.images[i]->resource == buffer.get());
            CHECK(objectName(buffer.get()) == std::wstring(label) + L".image" + std::to_wstring(i));
        }
    }

    //==================================================================================================================
    /// Runs one frame on `device` that acquires an image of `swapchain` and presents it with no work.
    void presentFrame(Device device, Swapchain swapchain)
    {
        device.beginFrame();
        REQUIRE(swapchain.acquire().bits != 0);
        device.queue(QueueType::Graphics).present(swapchain);
        device.endFrame();
    }
} // namespace

//======================================================================================================================
TEST_CASE("a swapchain's back buffers carry the format, count and flags of its descriptor", "[d3d12]")
{
    const MessageCollection messages;
    Device device = makeDevice();
    for (const uint32_t count : { 2u, 3u })
    {
        Swapchain swapchain = makeSwapchain(
            device, SwapchainDesc{ .nativeSurface = test::surface(count - 2), .imageCount = count, .maxLatency = count - 1, .label = "test.swapchain" }
        );
        d3d12::SwapchainNative& native = nativeOf(swapchain);
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        REQUIRE(SUCCEEDED(native.swapchain->GetDesc1(&desc)));
        CHECK(desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
        CHECK(desc.BufferCount == count);
        CHECK(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
        CHECK((desc.Flags & static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) != 0);
        CHECK((desc.Flags & static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) == 0);
        const Extent2D extent = SwapchainImpl::of(swapchain).core.extent;
        CHECK(extent.width > 0);
        CHECK(extent.height > 0);
        CHECK(desc.Width == extent.width);
        CHECK(desc.Height == extent.height);
        UINT latency = 0;
        REQUIRE(SUCCEEDED(native.swapchain->GetMaximumFrameLatency(&latency)));
        CHECK(latency == count - 1);
        CHECK(native.waitable != nullptr);
        REQUIRE(native.images.size() == count);
        checkBackBuffers(swapchain, L"test.swapchain");
    }
    destroyDevice(device);
    CHECK(messages.count() == 0);
}

//======================================================================================================================
TEST_CASE("each color space sets its DXGI color space", "[d3d12]")
{
    CHECK(d3d12::colorSpaceOf(ColorSpace::SRGB) == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    CHECK(d3d12::colorSpaceOf(ColorSpace::ExtendedLinear) == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    CHECK(d3d12::colorSpaceOf(ColorSpace::HDR10) == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    const MessageCollection messages;
    Device device = makeDevice();
    constexpr std::array<std::pair<ColorSpace, Format>, 3> kSpaces = {
        { { ColorSpace::SRGB, Format::BGRA8Unorm }, { ColorSpace::ExtendedLinear, Format::RGBA16Float }, { ColorSpace::HDR10, Format::RGB10A2Unorm } }
    };
    for (uint32_t i = 0; i < kSpaces.size(); ++i)
    {
        const auto [space, format] = kSpaces[i];
        Result<Swapchain> created =
            device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(i), .format = format, .colorSpace = space, .label = "test.swapchain" });
        if (created.has_value())
        {
            // DXGI reads no color space back, so the case checks that the output presents the one the swapchain was set to.
            d3d12::SwapchainNative& native = nativeOf(*created);
            UINT support = 0;
            REQUIRE(SUCCEEDED(native.swapchain->CheckColorSpaceSupport(d3d12::colorSpaceOf(space), &support)));
            CHECK((support & static_cast<UINT>(DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) != 0);
            DXGI_SWAP_CHAIN_DESC1 desc = {};
            REQUIRE(SUCCEEDED(native.swapchain->GetDesc1(&desc)));
            CHECK(desc.Format == d3d12::dxgiFormat(format));
        }
        else
        {
            WARN("the output refuses color space " << static_cast<int>(space) << ": " << created.error().message);
            CHECK(created.error().category == ErrorCategory::Unsupported);
            CHECK(created.error().message.find("ColorSpace::") != std::string::npos);
        }
    }
    destroyDevice(device);
    CHECK(messages.count() == 0);
}

//======================================================================================================================
TEST_CASE("presentFlags maps each mode and refuses Immediate without tearing", "[d3d12]")
{
    const auto waitable = static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    const auto tearing = static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    for (const bool supported : { false, true })
    {
        const Result<d3d12::PresentFlags> fifo = d3d12::presentFlags(PresentMode::Fifo, supported);
        REQUIRE(fifo.has_value());
        CHECK(fifo->interval == 1);
        CHECK(fifo->flags == 0);
        CHECK(fifo->swapchainFlags == waitable);
        const Result<d3d12::PresentFlags> mailbox = d3d12::presentFlags(PresentMode::Mailbox, supported);
        REQUIRE(mailbox.has_value());
        CHECK(mailbox->interval == 0);
        CHECK(mailbox->flags == 0);
        CHECK(mailbox->swapchainFlags == waitable);
    }
    const Result<d3d12::PresentFlags> immediate = d3d12::presentFlags(PresentMode::Immediate, true);
    REQUIRE(immediate.has_value());
    CHECK(immediate->interval == 0);
    CHECK(immediate->flags == static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING));
    CHECK(immediate->swapchainFlags == (waitable | tearing));
    const Result<d3d12::PresentFlags> refused = d3d12::presentFlags(PresentMode::Immediate, false);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().category == ErrorCategory::Unsupported);
    CHECK(refused.error().message.find("DXGI_FEATURE_PRESENT_ALLOW_TEARING") != std::string::npos);
}

//======================================================================================================================
TEST_CASE("acquire returns the current back buffer and present counts one submit call", "[d3d12]")
{
    const MessageCollection messages;
    Device device = makeDevice();
    Swapchain swapchain = makeSwapchain(device, SwapchainDesc{ .nativeSurface = test::surface(0), .label = "test.swapchain" });
    d3d12::SwapchainNative& native = nativeOf(swapchain);
    Queue graphics = device.queue(QueueType::Graphics);
    for (int frame = 0; frame < 5; ++frame)
    {
        device.beginFrame();
        const UINT current = native.swapchain->GetCurrentBackBufferIndex();
        const Counters before = device.counters();
        const Texture image = swapchain.acquire();
        const Counters acquired = device.counters();
        REQUIRE(image.bits != 0);
        // One wait on the frame-latency object; the index read is not counted.
        CHECK(acquired.nativeHostCalls - before.nativeHostCalls == 1);
        CHECK(acquired.nativeSubmitCalls == before.nativeSubmitCalls);
        const auto* payload = static_cast<const d3d12::TexturePayload*>(DeviceImpl::of(device).core.pool<Texture>().native(image));
        REQUIRE(payload != nullptr);
        CHECK(payload == native.images[current].get());
        {
            d3d12::Com<ID3D12Resource> buffer;
            REQUIRE(SUCCEEDED(native.swapchain->GetBuffer(current, IID_PPV_ARGS(buffer.put()))));
            CHECK(payload->resource == buffer.get());
        }
        CHECK(payload->targetCount == 1);
        graphics.present(swapchain);
        const Counters presented = device.counters();
        CHECK(presented.nativeSubmitCalls - acquired.nativeSubmitCalls == 1);
        CHECK(presented.nativeHostCalls == acquired.nativeHostCalls);
        CHECK(d3d12::presentSucceeded(native.presented));
        device.endFrame();
    }
    destroyDevice(device);
    CHECK(messages.count() == 0);
}

//======================================================================================================================
TEST_CASE("an occluded present succeeds", "[d3d12]")
{
    CHECK(d3d12::presentSucceeded(S_OK));
    CHECK(d3d12::presentSucceeded(DXGI_STATUS_OCCLUDED));
    CHECK_FALSE(d3d12::presentSucceeded(DXGI_STATUS_MODE_CHANGE_IN_PROGRESS));
    CHECK_FALSE(d3d12::presentSucceeded(DXGI_ERROR_INVALID_CALL));
    CHECK_FALSE(d3d12::presentSucceeded(DXGI_ERROR_DEVICE_REMOVED));
    const MessageCollection messages;
    const HWND window = static_cast<HWND>(test::surface(7));
    Device device = makeDevice();
    Swapchain swapchain = makeSwapchain(device, SwapchainDesc{ .nativeSurface = window, .label = "test.swapchain" });
    // A minimised window is not shown, which DXGI may report as DXGI_STATUS_OCCLUDED.
    ShowWindow(window, SW_SHOWMINNOACTIVE);
    std::vector<HRESULT> results;
    for (int frame = 0; frame < 3; ++frame)
    {
        device.beginFrame();
        // A frame whose acquire finds no image within its deadline presents nothing.
        if (swapchain.acquire().bits != 0)
        {
            device.queue(QueueType::Graphics).present(swapchain);
            results.push_back(nativeOf(swapchain).presented);
        }
        device.endFrame();
    }
    destroyDevice(device);
    // The surface goes back to a hidden window of its first extent for the cases that follow.
    ShowWindow(window, SW_SHOWNOACTIVATE);
    ShowWindow(window, SW_HIDE);
    // The first acquire always finds the frame-latency object signalled.
    REQUIRE_FALSE(results.empty());
    bool occluded = false;
    for (const HRESULT result : results)
    {
        CHECK(d3d12::presentSucceeded(result));
        occluded = occluded || result == DXGI_STATUS_OCCLUDED;
    }
    WARN("presents on a minimised window: " << results.size() << ", DXGI_STATUS_OCCLUDED " << (occluded ? "returned" : "not returned"));
    CHECK(messages.count() == 0);
}

//======================================================================================================================
TEST_CASE("resize drops every back-buffer reference before ResizeBuffers", "[d3d12]")
{
    const MessageCollection messages;
    Device device = makeDevice();
    const uint32_t framesInFlight = DeviceImpl::of(device).core.framesInFlight();
    Swapchain swapchain = makeSwapchain(device, SwapchainDesc{ .nativeSurface = test::surface(4), .label = "test.swapchain" });
    d3d12::SwapchainNative& native = nativeOf(swapchain);
    constexpr std::array<Extent2D, 3> kExtents = { { { 320, 200 }, { 64, 48 }, { 200, 320 } } };
    for (const Extent2D extent : kExtents)
    {
        // More than framesInFlight frames each round, so every frame slot holds presents of the images resized away.
        for (uint32_t frame = 0; frame <= framesInFlight; ++frame)
        {
            presentFrame(device, swapchain);
        }
        const Counters before = device.counters();
        swapchain.resize(extent);
        // The wait for the last ended frame reads the frame fence at least once.
        CHECK(device.counters().nativeHostCalls > before.nativeHostCalls);
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        REQUIRE(SUCCEEDED(native.swapchain->GetDesc1(&desc)));
        CHECK(desc.Width == extent.width);
        CHECK(desc.Height == extent.height);
        CHECK(desc.BufferCount == 3);
        CHECK((desc.Flags & static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) != 0);
        CHECK(SwapchainImpl::of(swapchain).core.extent.width == extent.width);
        checkBackBuffers(swapchain, L"test.swapchain");
    }
    // The swapchain is destroyed with an image acquired, inside the frame, and freed once a later beginFrame waits for it.
    device.beginFrame();
    REQUIRE(swapchain.acquire().bits != 0);
    const uint64_t retired = device.counters().deferredDestructionsRetired;
    device.destroy(swapchain);
    device.endFrame();
    for (uint32_t frame = 0; frame <= framesInFlight; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    CHECK(device.counters().deferredDestructionsRetired > retired);
    {
        d3d12::Presentation& presentation = DeviceImpl::of(device).presentation;
        const std::lock_guard lock(presentation.mutex);
        CHECK(presentation.swapchains.empty());
        CHECK(presentation.retired.empty());
    }
    // In a Debug build destroyDevice aborts when any reference to the device, a back buffer's among them, outlives it.
    destroyDevice(device);
    CHECK(messages.count() == 0);
}

//======================================================================================================================
TEST_CASE("a swapchain destroyed between frames stays until its last frame is waited for", "[d3d12]")
{
    const MessageCollection messages;
    Result<Device> created = createDevice(DeviceDesc{ .framesInFlight = 2 });
    REQUIRE(created.has_value());
    Device device = *created;
    Swapchain swapchain = makeSwapchain(device, SwapchainDesc{ .nativeSurface = test::surface(6), .label = "test.swapchain" });
    presentFrame(device, swapchain);
    presentFrame(device, swapchain);
    // Frame 2 has ended, and its presents may still run while frame 3 begins.
    device.destroy(swapchain);
    const auto retiredCount = [&] {
        d3d12::Presentation& presentation = DeviceImpl::of(device).presentation;
        const std::lock_guard lock(presentation.mutex);
        return presentation.retired.size();
    };
    // Filed with frame 2. Frame 3 waits for frame 1, not for frame 2.
    device.beginFrame();
    CHECK(retiredCount() == 1);
    device.endFrame();
    // Frame 4 waits for frame 2.
    device.beginFrame();
    CHECK(retiredCount() == 0);
    device.endFrame();
    destroyDevice(device);
    CHECK(messages.count() == 0);
}

//======================================================================================================================
TEST_CASE("a swapchain destroyed while beginFrame opens a frame outlives that call's free", "[d3d12]")
{
    const MessageCollection messages;
    Result<Device> created = createDevice(DeviceDesc{ .framesInFlight = 2 });
    REQUIRE(created.has_value());
    Device device = *created;
    DeviceImpl& impl = DeviceImpl::of(device);
    Swapchain swapchain = makeSwapchain(device, SwapchainDesc{ .nativeSurface = test::surface(6), .label = "test.swapchain" });
    presentFrame(device, swapchain);
    presentFrame(device, swapchain);
    const auto retiredCount = [&] {
        const std::lock_guard lock(impl.presentation.mutex);
        return impl.presentation.retired.size();
    };
    // Frame 3 opens before beginFrame frees, so a destruction racing that call sees frame 3, presents in it, and is filed
    // with it. The free that call runs, after waiting for frame 1, is replayed here and must keep the swapchain.
    device.beginFrame();
    REQUIRE(swapchain.acquire().bits != 0);
    device.queue(QueueType::Graphics).present(swapchain);
    device.destroy(swapchain);
    d3d12::freePassedSwapchains(impl, impl.core.frameNumber() - impl.core.framesInFlight());
    CHECK(retiredCount() == 1);
    device.endFrame();
    // Frame 4 waits for frame 2, and frame 5 for frame 3, which frees it.
    device.beginFrame();
    CHECK(retiredCount() == 1);
    device.endFrame();
    device.beginFrame();
    CHECK(retiredCount() == 0);
    device.endFrame();
    destroyDevice(device);
    CHECK(messages.count() == 0);
}

//======================================================================================================================
TEST_CASE("a destroyed swapchain's window takes a new swapchain before the old one retires", "[d3d12]")
{
    const MessageCollection messages;
    Device device = makeDevice();
    Swapchain first = makeSwapchain(device, SwapchainDesc{ .nativeSurface = test::surface(5), .label = "test.first" });
    presentFrame(device, first);
    presentFrame(device, first);
    // Destroyed outside a frame: its frame is waited for only frames later, so the new swapchain frees it first.
    device.destroy(first);
    const Counters before = device.counters();
    Swapchain second = makeSwapchain(device, SwapchainDesc{ .nativeSurface = test::surface(5), .label = "test.second" });
    CHECK(device.counters().deferredDestructionsRetired - before.deferredDestructionsRetired == 1);
    presentFrame(device, second);
    // Destroyed and replaced inside one open frame, after a present: the wait does not need the frame to end.
    device.beginFrame();
    REQUIRE(second.acquire().bits != 0);
    device.queue(QueueType::Graphics).present(second);
    device.destroy(second);
    Swapchain third = makeSwapchain(device, SwapchainDesc{ .nativeSurface = test::surface(5), .label = "test.third" });
    device.endFrame();
    presentFrame(device, third);
    checkBackBuffers(third, L"test.third");
    destroyDevice(device);
    CHECK(messages.count() == 0);
}

//======================================================================================================================
TEST_CASE("two swapchains acquire and present in one frame", "[d3d12]")
{
    const MessageCollection messages;
    Device device = makeDevice();
    Swapchain a = makeSwapchain(device, SwapchainDesc{ .nativeSurface = test::surface(2), .label = "test.a" });
    Swapchain b = makeSwapchain(device, SwapchainDesc{ .nativeSurface = test::surface(3), .imageCount = 2, .label = "test.b" });
    Queue graphics = device.queue(QueueType::Graphics);
    for (int frame = 0; frame < 4; ++frame)
    {
        device.beginFrame();
        const Texture imageA = a.acquire();
        const Texture imageB = b.acquire();
        REQUIRE(imageA.bits != 0);
        REQUIRE(imageB.bits != 0);
        CHECK(imageA != imageB);
        const Counters before = device.counters();
        graphics.present(a);
        graphics.present(b);
        CHECK(device.counters().nativeSubmitCalls - before.nativeSubmitCalls == 2);
        device.endFrame();
    }
    destroyDevice(device);
    CHECK(messages.count() == 0);
}
