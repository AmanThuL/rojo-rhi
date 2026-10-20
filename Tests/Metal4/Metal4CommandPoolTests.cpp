//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4CommandPoolTests.cpp
/// @brief Pins when the Metal 4 backend opens an autorelease pool around a native command: only under the validation layer.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The commands of each kind a case records at the smaller of its two sizes.
    constexpr uint32_t kCommands = 64;

    /// The edge of the render target, in texels.
    constexpr uint32_t kEdge = 64;

    /// Which count a case reads: every pool the thread opened, or the `CommandPool`s alone.
    enum class Counting
    {
        AllPools,
        CommandPools,
    };

    //==================================================================================================================
    /// Runs `record` with `count` on a list of the Graphics queue, then submits the list, waits for it and ends the frame.
    /// Returns how many pools `record` opened, counted as `counting` says.
    template<typename Record> uint64_t poolsOpenedBy(Device device, Record record, uint32_t count, Counting counting)
    {
        const uint64_t& counter = counting == Counting::AllPools ? metal4::poolsOpened() : metal4::commandPoolsOpened();
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        const uint64_t before = counter;
        record(list, count);
        const uint64_t opened = counter - before;
        list.end();
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(QueueType::Graphics).submit(std::array{ list }, {}, signal);
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
        return opened;
    }

    //==================================================================================================================
    /// Checks the pools of `kCommands` more commands of `record`: `perCommand` each under the validation layer, none without
    /// it. The opening of the list's encoder opens pools of its own, which the two sizes share, so only their difference
    /// counts.
    template<typename Record> void checkPoolsPerCommand(Device device, Record record, uint64_t perCommand, Counting counting = Counting::AllPools)
    {
        const uint64_t small = poolsOpenedBy(device, record, kCommands, counting);
        const uint64_t large = poolsOpenedBy(device, record, 2 * kCommands, counting);
        const uint64_t expected = DeviceImpl::of(device).commandPools ? perCommand * kCommands : 0;
        CHECK(large - small == expected);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a dispatch opens a command pool only under the validation layer", "[metal4]")
{
    Device device = *createDevice();
    const std::vector<std::byte> blob = test::standardLibrary();
    const ShaderLibrary library = *device.loadShaderLibrary(blob, "test.library");
    const Pipeline pipeline = *device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" }, .label = "test.compute" });
    checkPoolsPerCommand(
        device,
        [&](CommandList list, uint32_t count) {
            list.setPipeline(pipeline);
            for (uint32_t i = 0; i < count; ++i)
            {
                list.dispatch(1, 1, 1);
            }
        },
        1
    );
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a buffer copy and a fill open a command pool only under the validation layer", "[metal4]")
{
    Device device = *createDevice();
    const Buffer source = *device.createBuffer({ .size = 256, .label = "test.source" });
    const Buffer target = *device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.target" });
    checkPoolsPerCommand(
        device,
        [&](CommandList list, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i)
            {
                list.copyBuffer(source, 0, target, 0, 64);
            }
        },
        1
    );
    checkPoolsPerCommand(
        device,
        [&](CommandList list, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i)
            {
                list.fillBuffer(target, 0, 64, 0xABu * 0x01010101u);
            }
        },
        1
    );
    // A fill whose pattern is not one byte dispatches the fill kernel, one dispatch per call for this size.
    checkPoolsPerCommand(
        device,
        [&](CommandList list, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i)
            {
                list.fillBuffer(target, 0, 64, 0x01020304u);
            }
        },
        1
    );
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a draw opens a command pool only under the validation layer", "[metal4]")
{
    Device device = *createDevice();
    const std::vector<std::byte> blob = test::standardLibrary();
    const ShaderLibrary library = *device.loadShaderLibrary(blob, "test.library");
    const std::array colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Pipeline pipeline = *device.createPipeline(
        GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .colors = colors, .label = "test.pipeline" }
    );
    const Texture target = *device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
    );
    const std::array<uint16_t, 3> values{ 0, 1, 2 };
    const Buffer indices =
        *device.createBuffer(BufferDesc{ .size = sizeof(values), .usage = BufferUsage::Index, .label = "test.indices" }, std::as_bytes(std::span(values)));
    checkPoolsPerCommand(
        device,
        [&](CommandList list, uint32_t count) {
            const std::array attachments{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
            list.beginRenderPass(RenderPassDesc{ .colors = attachments, .area = { 0, 0, kEdge, kEdge }, .label = "test.pass" });
            list.setViewport(Viewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) });
            list.setScissor(Rect2D{ 0, 0, kEdge, kEdge });
            list.setPipeline(pipeline);
            for (uint32_t i = 0; i < count; ++i)
            {
                list.draw(3, 1, 0, 0);
                list.drawIndexed(indices, IndexType::UInt16, 3, 1, 0, 0, 0);
            }
            list.endRenderPass();
        },
        2
    );
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the detected flag matches the layer the process runs under", "[metal4]")
{
    Device device = *createDevice();
    // The test, not the library, reads the environment: the layer is on exactly when MTL_DEBUG_LAYER is set to 1. A bare
    // driver device is the only one whose commands open no pool.
    const char* layer = std::getenv("MTL_DEBUG_LAYER");
    const bool layerOn = layer != nullptr && std::strcmp(layer, "1") == 0;
    CHECK(DeviceImpl::of(device).commandPools == layerOn);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a scope between encoders and a render pass open their command pools only under the validation layer", "[metal4]")
{
    Device device = *createDevice();
    // A scope writes a timestamp on the command buffer at its begin and at its end, between encoders.
    checkPoolsPerCommand(
        device,
        [&](CommandList list, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i)
            {
                list.beginScope("test.scope");
                list.endScope();
            }
        },
        2,
        Counting::CommandPools
    );
    const Texture target = *device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
    );
    // A pass writes its two timestamps on its render encoder, which opens no pool, and ends that encoder once.
    checkPoolsPerCommand(
        device,
        [&](CommandList list, uint32_t count) {
            for (uint32_t i = 0; i < count; ++i)
            {
                const std::array attachments{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
                list.beginRenderPass(RenderPassDesc{ .colors = attachments, .area = { 0, 0, kEdge, kEdge }, .label = "test.pass" });
                list.endRenderPass();
            }
        },
        1,
        Counting::CommandPools
    );
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("counted draws open their kernel and replay command pools only under the validation layer", "[metal4]")
{
    constexpr uint32_t kRecords = 8;
    Device device = *createDevice();
    const std::vector<std::byte> blob = test::standardLibrary();
    const ShaderLibrary library = *device.loadShaderLibrary(blob, "test.library");
    const std::array colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Pipeline pipeline = *device.createPipeline(
        GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .colors = colors, .label = "test.pipeline" }
    );
    const Texture target = *device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
    );
    std::vector<DrawIndirectArgs> draws;
    for (uint32_t i = 0; i < kRecords; ++i)
    {
        draws.push_back(DrawIndirectArgs{ .drawId = i, .vertexCount = 3, .instanceCount = 1 });
    }
    const Buffer records = *device.createBuffer(
        BufferDesc{ .size = sizeof(DrawIndirectArgs) * kRecords, .usage = BufferUsage::IndirectArgs, .label = "test.records" }, std::as_bytes(std::span(draws))
    );
    const std::array<uint32_t, 1> many{ kRecords };
    const Buffer counts =
        *device.createBuffer(BufferDesc{ .size = sizeof(many), .usage = BufferUsage::IndirectArgs, .label = "test.counts" }, std::as_bytes(std::span(many)));
    // Inside one pass, each counted draw runs a kernel dispatch in the batch form and replays the commands it wrote with
    // executeCommandsInBuffer: two command pools.
    checkPoolsPerCommand(
        device,
        [&](CommandList list, uint32_t count) {
            const std::array attachments{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
            list.beginRenderPass(RenderPassDesc{ .colors = attachments, .area = { 0, 0, kEdge, kEdge }, .label = "test.pass" });
            list.setViewport(Viewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) });
            list.setScissor(Rect2D{ 0, 0, kEdge, kEdge });
            list.setPipeline(pipeline);
            for (uint32_t i = 0; i < count; ++i)
            {
                list.drawIndirect(device.address(records), kRecords, device.address(counts));
            }
            list.endRenderPass();
        },
        2,
        Counting::CommandPools
    );
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("mesh draws open a command pool only under the validation layer", "[metal4]")
{
    Device device = *createDevice();
    if (!device.capabilities().meshTasksIndirect)
    {
        destroyDevice(device);
        SKIP("the device has no mesh shaders");
    }
    const std::vector<std::byte> blob = test::standardLibrary();
    const ShaderLibrary library = *device.loadShaderLibrary(blob, "test.library");
    const std::array colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Pipeline pipeline = *device.createPipeline(
        GraphicsPipelineDesc{ .mesh = { library, "msMain" }, .fragment = { library, "fsMain" }, .colors = colors, .label = "test.mesh" }
    );
    const Texture target = *device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
    );
    const std::array<DrawMeshTasksIndirectArgs, 1> args{ DrawMeshTasksIndirectArgs{ .drawId = 0, .x = 1, .y = 1, .z = 1 } };
    const Buffer records =
        *device.createBuffer(BufferDesc{ .size = sizeof(args), .usage = BufferUsage::IndirectArgs, .label = "test.records" }, std::as_bytes(std::span(args)));
    const auto inPass = [&](auto draw) {
        return [&, draw](CommandList list, uint32_t count) {
            const std::array attachments{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
            list.beginRenderPass(RenderPassDesc{ .colors = attachments, .area = { 0, 0, kEdge, kEdge }, .label = "test.pass" });
            list.setViewport(Viewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) });
            list.setScissor(Rect2D{ 0, 0, kEdge, kEdge });
            list.setPipeline(pipeline);
            for (uint32_t i = 0; i < count; ++i)
            {
                draw(list);
            }
            list.endRenderPass();
        };
    };
    // A direct mesh draw and a lone indirect record each send one native draw.
    checkPoolsPerCommand(device, inPass([](CommandList list) { list.drawMeshTasks(1, 1, 1); }), 1, Counting::CommandPools);
    checkPoolsPerCommand(device, inPass([&](CommandList list) { list.drawMeshTasksIndirect(device.address(records), 1); }), 1, Counting::CommandPools);
    destroyDevice(device);
}
