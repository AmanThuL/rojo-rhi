//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4CopyTests.cpp
/// @brief Tests the native calls Metal 4 copies and fills record on the lazily opened compute encoder.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Encoders.h"
#include "Support/Metal4/Metal4Timing.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// The encoders of `list`.
    const metal4::Encoders& encodersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders;
    }

    //==================================================================================================================
    /// What `list` has counted so far; fails the case when a scope it recorded holds no timestamp pair, since the pinned
    /// counts include both timestamp writes.
    const core::RecordCounts& countsOf(CommandList list)
    {
        return test::timedCounts(list);
    }

    //==================================================================================================================
    /// The label of the compute encoder `list` has open, read inside a pool because the getter autoreleases.
    std::string encoderLabel(CommandList list)
    {
        MTL4::ComputeCommandEncoder* encoder = encodersOf(list).computeEncoder();
        REQUIRE(encoder != nullptr);
        const metal4::AutoreleasePool pool;
        return metal4::toStdString(encoder->label());
    }

    //==================================================================================================================
    /// Ends `list`, submits it to the queue of `type`, waits for it on the host and ends the frame.
    void finish(Device device, CommandList list, QueueType type = QueueType::Graphics)
    {
        list.end();
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(type).submit(std::array{ list }, {}, signal);
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
    }

    //==================================================================================================================
    /// Creates a texture of `type` with `layers` layers (or `depth` slices) of 8 by 8 RGBA8Unorm texels.
    Texture texture8(Device device, TextureType type, uint32_t layers, uint32_t depth = 1)
    {
        return *device.createTexture(
            TextureDesc{ .type = type,
                         .format = Format::RGBA8Unorm,
                         .extent = { 8, 8, depth },
                         .layers = layers,
                         .usage = TextureUsage::Sampled,
                         .label = "test.texture" }
        );
    }
} // namespace

//======================================================================================================================
TEST_CASE("the first copy opens a compute encoder labelled after the list and bound to its table", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    const CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    CHECK_FALSE(encodersOf(list).holdsWork());
    CHECK(countsOf(list).nativeRecordCalls == 2);
    CommandList(list).copyBuffer(a, 0, b, 0, 256);
    REQUIRE(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    REQUIRE(encodersOf(list).computeEncoder() != nullptr);
    CHECK(encodersOf(list).holdsWork());
    CHECK(countsOf(list).encodersOpened == 1);
    // computeCommandEncoder, setLabel and setArgumentTable open it; copyFromBuffer is the copy.
    CHECK(countsOf(list).nativeRecordCalls == 2 + 2 + label + 1);
    CHECK(encoderLabel(list) == std::string(label != 0 ? "test.list" : ""));
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("copies share one encoder until a scope boundary closes it, and the next encoder takes the scope's label", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Compute, "test.list");
    const uint64_t label = test::encoderLabel(list);
    list.copyBuffer(a, 0, b, 0, 64);
    list.copyBuffer(a, 64, b, 64, 64);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeRecordCalls == 2 + 2 + label + 2);
    // The scope closes the encoder (endEncoding) before its debug group and its begin timestamp.
    list.beginScope("test.scope");
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    CHECK_FALSE(encodersOf(list).holdsWork());
    CHECK(countsOf(list).nativeRecordCalls == 6 + label + 3);
    list.copyBuffer(a, 128, b, 128, 64);
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(encoderLabel(list) == std::string(label != 0 ? "test.scope" : ""));
    list.copyBuffer(a, 192, b, 192, 64);
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(countsOf(list).nativeRecordCalls == 9 + label + 2 + label + 2);
    list.endScope();
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    CHECK(countsOf(list).nativeRecordCalls == 13 + 2 * label + 3);
    list.copyBuffer(a, 0, b, 0, 64);
    CHECK(countsOf(list).encodersOpened == 3);
    CHECK(encoderLabel(list) == std::string(label != 0 ? "test.list" : ""));
    finish(device, list, QueueType::Compute);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an equal-byte fill is one native fillBuffer", "[metal4]")
{
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.fill" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    list.fillBuffer(buffer, 0, 128, 0x5A5A5A5Au);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeRecordCalls == 2 + 2 + label + 1);
    list.fillBuffer(buffer, 128, 128, 0u);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(countsOf(list).nativeRecordCalls == 5 + label + 1);
    CHECK(encodersOf(list).holdsWork());
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a three-layer buffer-to-texture copy is three native copies and a 3D copy is one", "[metal4]")
{
    Device device = *createDevice();
    const Texture array = texture8(device, TextureType::Tex2DArray, 3);
    const Texture volume = texture8(device, TextureType::Tex3D, 1, 4);
    const Buffer upload = *device.createBuffer({ .size = 4 * 2048, .memory = Memory::Upload, .label = "test.upload" });
    const Buffer readback = *device.createBuffer({ .size = 2 * 2048, .memory = Memory::Readback, .label = "test.readback" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    const BufferTextureLayout layout{ .offset = 0, .rowPitch = 256, .rowsPerImage = 8 };
    list.copyBufferToTexture(upload, layout, array, TextureRegion{ .layer = 0, .layers = 3, .extent = { 8, 8, 1 } });
    CHECK(countsOf(list).nativeRecordCalls == 2 + 2 + label + 3);
    list.copyBufferToTexture(upload, layout, volume, TextureRegion{ .extent = { 8, 8, 4 } });
    CHECK(countsOf(list).nativeRecordCalls == 7 + label + 1);
    // The texture-to-buffer direction splits the same way.
    list.copyTextureToBuffer(array, TextureRegion{ .layer = 1, .layers = 2, .extent = { 8, 8, 1 } }, readback, layout);
    CHECK(countsOf(list).nativeRecordCalls == 8 + label + 2);
    CHECK(countsOf(list).encodersOpened == 1);
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("end closes the open encoder before it ends the command buffer", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Copy, "test.list");
    list.copyBuffer(a, 0, b, 0, 256);
    const uint64_t before = countsOf(list).nativeRecordCalls;
    finish(device, list, QueueType::Copy);
    // endEncoding, then endCommandBuffer; the submission completed under the validation layer.
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    CHECK(encodersOf(list).computeEncoder() == nullptr);
    CHECK(countsOf(list).nativeRecordCalls == before + 2);
    CHECK(countsOf(list).encodersOpened == 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("lists recorded on other threads than their acquisition each open their own encoder", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 4096, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 4096, .label = "test.b" });
    device.beginFrame();
    std::array<CommandList, 4> lists{};
    for (CommandList& list : lists)
    {
        list = device.acquireCommandList(QueueType::Graphics, "test.list");
    }
    std::vector<std::thread> threads;
    for (size_t i = 0; i < lists.size(); ++i)
    {
        threads.emplace_back([&lists, a, b, i] {
            for (uint64_t copy = 0; copy < 16; ++copy)
            {
                const uint64_t offset = (i * 16 + copy) * 64;
                lists[i].copyBuffer(a, offset, b, offset, 64);
            }
            lists[i].end();
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    for (CommandList list : lists)
    {
        CHECK(countsOf(list).encodersOpened == 1);
        CHECK(countsOf(list).nativeRecordCalls == 2 + 2 + test::encoderLabel(list) + 16 + 2);
    }
    CHECK(encodersOf(lists[0]).computeEncoder() == nullptr);
    const Fence fence = *device.createFence(0, "test.done");
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    device.queue(QueueType::Graphics).submit(lists, {}, signal);
    CHECK(device.wait({ fence, 1 }, UINT64_MAX));
    device.endFrame();
    destroyDevice(device);
}
