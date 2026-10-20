//----------------------------------------------------------------------------------------------------------------------
/// @file DeviceTests.cpp
/// @brief Tests device creation, capabilities, frames and deferred destruction against chapter 02.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    struct Message
    {
        MessageSeverity severity;
        std::string text;
    };
    std::vector<Message> gMessages;

    //==================================================================================================================
    void collectMessage(MessageSeverity severity, std::string_view message, void*)
    {
        gMessages.push_back({ severity, std::string(message) });
    }

    //==================================================================================================================
    Device makeDevice(DeviceDesc desc = {})
    {
        Result<Device> created = createDevice(desc);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    uint32_t slotOf(Buffer buffer)
    {
        return static_cast<uint32_t>(buffer.bits);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a default DeviceDesc creates a device with three frame slots", "[contract]")
{
    Device device = makeDevice();
    CHECK(core::coreOf(device).framesInFlight() == 3);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two and three frame slots are both accepted", "[contract]")
{
    for (const uint32_t slots : { 2u, 3u })
    {
        Device device = makeDevice({ .framesInFlight = slots });
        CHECK(core::coreOf(device).framesInFlight() == slots);
        destroyDevice(device);
    }
}

//======================================================================================================================
TEST_CASE("an empty device label becomes rojorhi.device.unnamed", "[contract]")
{
    Device device = makeDevice();
    CHECK(core::coreOf(device).label() == "rojorhi.device.unnamed");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a device label is kept verbatim", "[contract]")
{
    Device device = makeDevice({ .label = "my.Device \xC3\xA9" });
    CHECK(core::coreOf(device).label() == "my.Device \xC3\xA9");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("several devices coexist with independent frames", "[contract]")
{
    Device first = makeDevice();
    Device second = makeDevice();
    first.beginFrame();
    CHECK(core::coreOf(first).frameOpen());
    CHECK_FALSE(core::coreOf(second).frameOpen());
    first.endFrame();
    destroyDevice(first);
    destroyDevice(second);
}

//======================================================================================================================
TEST_CASE("a pipeline cache blob is accepted and ignored", "[contract]")
{
    const std::byte blob[4] = {};
    Device device = makeDevice({ .pipelineCache = blob });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("validation is unsupported in Release and creates a device in Debug", "[contract]")
{
    Result<Device> created = createDevice({ .validation = true });
#ifdef NDEBUG
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().category == ErrorCategory::Unsupported);
    CHECK_FALSE(created.error().message.empty());
#else
    REQUIRE(created.has_value());
    CHECK(core::coreOf(*created).validation());
    destroyDevice(*created);
#endif
}

//======================================================================================================================
TEST_CASE("capabilities never list Auto as an upscaler and name the device", "[contract]")
{
    Device device = makeDevice();
    const Capabilities& caps = device.capabilities();
    CHECK_FALSE(caps.adapterName.empty());
    CHECK_FALSE(caps.floorName.empty());
    for (const UpscalerKind kind : caps.upscalers)
    {
        CHECK(kind != UpscalerKind::Auto);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("every limit is at least the floor of the reference", "[contract]")
{
    Device device = makeDevice();
    const Capabilities& caps = device.capabilities();
    const Limits& limits = caps.limits;
    CHECK(caps.maxViews >= 1'000'000);
    CHECK(caps.maxSamplers >= 996);
    CHECK(limits.maxViews >= 1'000'000);
    CHECK(limits.maxSamplers >= 996);
    CHECK(limits.maxRootBlockSize >= 65'536);
    CHECK(limits.rootAlignment >= 256);
    CHECK(limits.frameDataAlignment >= 256);
    CHECK(limits.maxAddrOffset >= 536'870'911ull);
    CHECK(limits.maxTextureDimension1D >= 16'384);
    CHECK(limits.maxTextureDimension2D >= 16'384);
    CHECK(limits.maxTextureDimension3D >= 2'048);
    CHECK(limits.maxTextureDimensionCube >= 16'384);
    CHECK(limits.maxTextureLayers >= 2'048);
    CHECK(limits.maxColorAttachments >= 8);
    CHECK(limits.maxColorBytesPerSample >= 64);
    CHECK(limits.maxSamplerAnisotropy >= 16);
    CHECK(limits.maxDrawIndirectCount >= 65'535);
    CHECK(limits.maxDispatchGroups >= 65'535);
    CHECK(limits.maxThreadsPerGroup >= 1'024);
    CHECK(limits.maxGroupSize[0] >= 1'024);
    CHECK(limits.maxGroupSize[1] >= 1'024);
    CHECK(limits.maxGroupSize[2] >= 64);
    CHECK(limits.maxGroupSharedMemory >= 32'768);
    CHECK(limits.maxMeshTaskGroups >= 65'535);
    CHECK(limits.maxMeshTaskGroupsTotal >= 4'194'304);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("capabilities returns the same object on every call", "[contract]")
{
    Device device = makeDevice();
    CHECK(&device.capabilities() == &device.capabilities());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("frames open and close in sequence", "[contract]")
{
    Device device = makeDevice();
    core::DeviceCore& core = core::coreOf(device);
    CHECK(core.frameNumber() == 0);
    for (uint64_t frame = 1; frame <= 7; ++frame)
    {
        device.beginFrame();
        CHECK(core.frameOpen());
        CHECK(core.frameNumber() == frame);
        device.endFrame();
        CHECK_FALSE(core.frameOpen());
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a destroyed handle is rejected at once and its slot is reused after the frame retires", "[contract]")
{
    Device device = makeDevice({ .framesInFlight = 2 });
    auto& pool = core::coreOf(device).pool<Buffer>();
    device.beginFrame(); // frame 1
    const Buffer buffer = pool.allocate({ "rojorhi.buffer.unnamed" });
    device.destroy(buffer);
    CHECK(pool.lookup(buffer).status == core::LookupStatus::StaleGeneration);
    CHECK(slotOf(pool.allocate({})) != slotOf(buffer));
    device.endFrame();

    device.beginFrame(); // frame 2: the slot of frame 1 is still in flight
    CHECK(slotOf(pool.allocate({})) != slotOf(buffer));
    device.endFrame();

    device.beginFrame(); // frame 3 retires frame 1
    CHECK(slotOf(pool.allocate({})) == slotOf(buffer));
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an object destroyed outside a frame is released with the next frame opened", "[contract]")
{
    Device device = makeDevice({ .framesInFlight = 2 });
    auto& pool = core::coreOf(device).pool<Texture>();
    const Texture texture = pool.allocate({});
    device.destroy(texture);
    const auto occupied = [&] { return static_cast<uint32_t>(pool.allocate({}).bits); };

    device.beginFrame(); // frame 1
    CHECK(occupied() != static_cast<uint32_t>(texture.bits));
    device.endFrame();
    device.beginFrame(); // frame 2
    CHECK(occupied() != static_cast<uint32_t>(texture.bits));
    device.endFrame();
    device.beginFrame(); // frame 3 retires frame 1
    CHECK(occupied() == static_cast<uint32_t>(texture.bits));
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("destroy accepts a live handle of every resource type", "[contract]")
{
    Device device = makeDevice();
    core::DeviceCore& core = core::coreOf(device);
    const Pipeline pipeline = core.pool<Pipeline>().allocate({});
    const ShaderLibrary library = core.pool<ShaderLibrary>().allocate({});
    const Heap heap = core.pool<Heap>().allocate({});
    const Fence fence = core.pool<Fence>().allocate({});
    const Upscaler upscaler = core.pool<Upscaler>().allocate({});
    device.destroy(pipeline);
    device.destroy(library);
    device.destroy(heap);
    device.destroy(fence);
    device.destroy(upscaler);
    CHECK(core.pool<Pipeline>().liveCount() == 0);
    CHECK(core.pool<ShaderLibrary>().liveCount() == 0);
    CHECK(core.pool<Heap>().liveCount() == 0);
    CHECK(core.pool<Fence>().liveCount() == 0);
    CHECK(core.pool<Upscaler>().liveCount() == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("destroy accepts a view index createView returned and a sampler index on a live device", "[contract]")
{
    Device device = makeDevice();
    const Texture texture = *device.createTexture({ .format = Format::R8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    device.destroy(*device.createView(texture, {}));
    device.destroy(texture);
    CHECK(core::coreOf(device).views().assignedCount() == 0);
    device.destroy(*device.createSampler({}));
    CHECK(core::coreOf(device).samplers().assignedCount() == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("destroyDevice with validation warns of each live object by label", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    gMessages.clear();
    setMessageCallback(collectMessage, nullptr);
    Device device = makeDevice({ .validation = true });
    core::DeviceCore& core = core::coreOf(device);
    core.pool<Buffer>().allocate({ "scene.vertices" });
    core.pool<Fence>().allocate({ "scene.fence" });
    destroyDevice(device);
    setMessageCallback(nullptr, nullptr);

    REQUIRE(gMessages.size() == 2);
    CHECK(gMessages[0].severity == MessageSeverity::Warning);
    CHECK(gMessages[1].severity == MessageSeverity::Warning);
    CHECK(gMessages[0].text.find("scene.vertices") != std::string::npos);
    CHECK(gMessages[1].text.find("scene.fence") != std::string::npos);
#endif
}

//======================================================================================================================
TEST_CASE("destroyDevice without validation reports nothing", "[contract]")
{
    gMessages.clear();
    setMessageCallback(collectMessage, nullptr);
    Device device = makeDevice();
    core::coreOf(device).pool<Buffer>().allocate({ "scene.vertices" });
    destroyDevice(device);
    setMessageCallback(nullptr, nullptr);
    CHECK(gMessages.empty());
}

//======================================================================================================================
TEST_CASE("destroying from another thread while frames run releases every handle", "[contract]")
{
    Device device = makeDevice();
    auto& pool = core::coreOf(device).pool<Buffer>();
    std::vector<Buffer> buffers;
    for (int i = 0; i < 2000; ++i)
    {
        buffers.push_back(pool.allocate({}));
    }
    {
        std::jthread frames([&] {
            for (int i = 0; i < 2000; ++i)
            {
                device.beginFrame();
                device.endFrame();
            }
        });
        std::jthread destroyer([&] {
            for (const Buffer buffer : buffers)
            {
                device.destroy(buffer);
            }
        });
    }
    for (const Buffer buffer : buffers)
    {
        CHECK(pool.lookup(buffer).status == core::LookupStatus::StaleGeneration);
    }
    CHECK(pool.liveCount() == 0);
    destroyDevice(device);
}
