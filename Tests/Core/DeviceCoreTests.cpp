//----------------------------------------------------------------------------------------------------------------------
/// @file DeviceCoreTests.cpp
/// @brief Tests the per-type records, the retire hook and the capability tables the core keeps for a backend.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>
#include <vector>

using namespace rojoRHI;

namespace {
    static_assert(!std::is_same_v<core::RecordFor<Buffer>, core::RecordFor<Texture>>);
    static_assert(std::is_same_v<core::RecordFor<Buffer>, core::BufferRecord>);
    static_assert(std::is_same_v<core::RecordFor<Upscaler>, core::UpscalerRecord>);

    /// What the hooks saw: the labels of retired buffers and textures, in order.
    struct Seen
    {
        std::vector<std::string> buffers;
        std::vector<std::string> textures;
    };

    //==================================================================================================================
    void onBuffer(void* context, core::BufferRecord& record)
    {
        static_cast<Seen*>(context)->buffers.push_back(record.label);
    }

    //==================================================================================================================
    void onTexture(void* context, core::TextureRecord& record)
    {
        static_cast<Seen*>(context)->textures.push_back(record.label);
    }

    //==================================================================================================================
    core::RetireHooks hooksFor(Seen& seen)
    {
        core::RetireHooks hooks;
        hooks.context = &seen;
        hooks.buffer = onBuffer;
        hooks.texture = onTexture;
        return hooks;
    }

    //==================================================================================================================
    void runFrames(Device& device, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            device.beginFrame();
            device.endFrame();
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("the record of a Buffer and of a Texture are separate types a backend fills", "[core]")
{
    core::BufferRecord buffer{ "a" };
    core::TextureRecord texture{ "b" };
    int payload = 0;
    buffer.native = &payload;
    CHECK(buffer.label == "a");
    CHECK(texture.label == "b");
    CHECK(texture.native == nullptr);
    CHECK(buffer.native == &payload);
}

//======================================================================================================================
TEST_CASE("the retire hook sees each released record exactly once, at the frame it retires", "[core]")
{
    Seen seen;
    Device device = *createDevice({ .framesInFlight = 2 });
    core::DeviceCore& core = core::coreOf(device);
    core.setRetireHooks(hooksFor(seen));
    const Buffer buffer = core.pool<Buffer>().allocate({ "test.buffer" });
    const Texture texture = core.pool<Texture>().allocate({ "test.texture" });
    device.destroy(buffer); // No frame is open, so it is released with frame 1 and retires when frame 3 begins.
    CHECK(seen.buffers.empty());
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    CHECK(seen.buffers.empty());
    device.beginFrame(); // Frame 3.
    REQUIRE(seen.buffers.size() == 1);
    CHECK(seen.buffers[0] == "test.buffer");
    device.endFrame();
    runFrames(device, 4);
    CHECK(seen.buffers.size() == 1);
    CHECK(seen.textures.empty());
    device.destroy(texture);
    destroyDevice(device);
    CHECK(seen.buffers.size() == 1);
    CHECK(seen.textures.size() == 1);
}

//======================================================================================================================
TEST_CASE("destroying a device drains live and pending records through the hook once", "[core]")
{
    Seen seen;
    Device device = *createDevice();
    core::DeviceCore& core = core::coreOf(device);
    core.setRetireHooks(hooksFor(seen));
    const Buffer released = core.pool<Buffer>().allocate({ "pending.buffer" });
    core.pool<Buffer>().allocate({ "live.buffer" });
    core.pool<Texture>().allocate({ "live.texture" });
    device.destroy(released);
    destroyDevice(device);
    CHECK(seen.buffers.size() == 2);
    CHECK(seen.textures == std::vector<std::string>{ "live.texture" });
}

//======================================================================================================================
TEST_CASE("a type with no hook retires its record without one", "[core]")
{
    Seen seen;
    Device device = *createDevice({ .framesInFlight = 2 });
    core::DeviceCore& core = core::coreOf(device);
    core.setRetireHooks(hooksFor(seen)); // Buffer and Texture have hooks; Fence has none.
    const Fence fence = core.pool<Fence>().allocate({ "test.fence" });
    device.destroy(fence);
    runFrames(device, 3);
    CHECK(core.pool<Fence>().empty());
    CHECK(seen.buffers.empty());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the core is empty by the time it is destroyed", "[core]")
{
    Seen seen;
    Device device = *createDevice();
    core::DeviceCore& core = core::coreOf(device);
    core.setRetireHooks(hooksFor(seen));
    core.pool<Buffer>().allocate({ "live.buffer" });
    destroyDevice(device);
    CHECK(seen.buffers.size() == 1);
}

//======================================================================================================================
TEST_CASE("a type with no hook is released without one", "[core]")
{
    Device device = *createDevice();
    core::DeviceCore& core = core::coreOf(device);
    core.setRetireHooks({});
    core.pool<Fence>().allocate({ "test.fence" });
    runFrames(device, 5);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("capability and format queries read the tables the core keeps", "[core]")
{
    Device device = *createDevice();
    core::DeviceCore& core = core::coreOf(device);
    CHECK(&device.capabilities() == &core.capabilities());
    core.setFormatCapabilities(Format::R8Unorm, FormatCapabilities{ .sampled = true });
    const FormatCapabilities caps = device.formatCapabilities(Format::R8Unorm);
    CHECK(caps.sampled);
    CHECK_FALSE(caps.storageRead);
    CHECK(core.formatCapabilities(Format::R8Unorm).sampled);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("destroying a core that was never drained calls no hook", "[core]")
{
#ifdef NDEBUG
    // A Debug build asserts instead, because a skipped beginDestroy is a backend bug.
    Seen seen;
    {
        core::DeviceCore core{ DeviceDesc{} };
        core.setRetireHooks(hooksFor(seen));
        core.pool<Buffer>().allocate({ "live.buffer" });
    }
    CHECK(seen.buffers.empty());
#else
    SUCCEED("the Debug build asserts that every pool is drained");
#endif
}
