//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ClearTests.cpp
/// @brief Tests the D3D12 texture clear: its view kinds, its bracket of transitions, its counts and the texels it writes.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12Kernels.h"
#include "D3D12ListState.h"
#include "D3D12Textures.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    /// A clear colour whose RGBA8 encoding is exact: 51, 102, 153 and 204.
    constexpr ClearColor kFirst = { 0.2f, 0.4f, 0.6f, 0.8f };

    /// A second clear colour, the first reversed.
    constexpr ClearColor kSecond = { 0.8f, 0.6f, 0.4f, 0.2f };

    /// A third clear colour: 0, 255, 51 and 255.
    constexpr ClearColor kThird = { 0.0f, 1.0f, 0.2f, 1.0f };

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
    /// Returns a new device with the default description.
    Device makeDevice()
    {
        Result<Device> created = createDevice();
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new fence at zero on `device`, labelled `label`.
    Fence makeFence(Device device, std::string_view label)
    {
        Result<Fence> created = device.createFence(0, label);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new texture of `desc` on `device`, placed by `placement`.
    Texture makeTexture(Device device, const TextureDesc& desc, Placement placement = {})
    {
        Result<Texture> created = device.createTexture(desc, {}, placement);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns the payload of `texture`, a live texture of `device`.
    const d3d12::TexturePayload& payloadOf(Device device, Texture texture)
    {
        const auto* payload = static_cast<const d3d12::TexturePayload*>(DeviceImpl::of(device).core.pool<Texture>().native(texture));
        REQUIRE(payload != nullptr);
        REQUIRE(payload->resource != nullptr);
        return *payload;
    }

    //==================================================================================================================
    /// Returns the native buffer of `buffer`, a live buffer of `device`.
    ID3D12Resource* resourceOf(Device device, Buffer buffer)
    {
        const auto* payload = static_cast<const d3d12::BufferPayload*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
        REQUIRE(payload != nullptr);
        return payload->resource;
    }

    //==================================================================================================================
    /// Returns the native calls `list` has recorded since its acquisition.
    uint64_t recordCalls(CommandList list)
    {
        return core::coreOf(list).counts.nativeRecordCalls;
    }

    //==================================================================================================================
    /// Returns the native barrier commands `list` has recorded since its acquisition.
    uint64_t nativeBarriers(CommandList list)
    {
        return core::coreOf(list).counts.nativeBarriers;
    }

    //==================================================================================================================
    /// Returns the descriptors `device` has written since its creation.
    uint64_t descriptorWrites(Device device)
    {
        return device.counters().descriptorWrites;
    }

    //==================================================================================================================
    /// Returns a barrier that moves `range` of `texture` from `Undefined` into `CopyDst` for a clear.
    TextureBarrier enter(Texture texture, SubresourceRange range = {})
    {
        return { .texture = texture,
                 .range = range,
                 .srcStage = Stage::None,
                 .srcAccess = Access::None,
                 .dstStage = Stage::Copy,
                 .dstAccess = Access::CopyWrite,
                 .oldLayout = Layout::Undefined,
                 .newLayout = Layout::CopyDst };
    }

    //==================================================================================================================
    /// Returns a barrier that orders a copy write of `range` of `texture` before another, both in `CopyDst`.
    TextureBarrier rewrite(Texture texture, SubresourceRange range = {})
    {
        return { .texture = texture,
                 .range = range,
                 .srcStage = Stage::Copy,
                 .srcAccess = Access::CopyWrite,
                 .dstStage = Stage::Copy,
                 .dstAccess = Access::CopyWrite,
                 .oldLayout = Layout::CopyDst,
                 .newLayout = Layout::CopyDst };
    }

    //==================================================================================================================
    /// Returns a barrier that moves `range` of `texture` from `CopyDst`, after a clear, into `CopySrc`.
    TextureBarrier toCopySrc(Texture texture, SubresourceRange range = {})
    {
        return { .texture = texture,
                 .range = range,
                 .srcStage = Stage::Copy,
                 .srcAccess = Access::CopyWrite,
                 .dstStage = Stage::Copy,
                 .dstAccess = Access::CopyRead,
                 .oldLayout = Layout::CopyDst,
                 .newLayout = Layout::CopySrc };
    }

    //==================================================================================================================
    /// Records `barrier` on `list` as a batch of its own.
    void record(CommandList list, const TextureBarrier& barrier)
    {
        list.barrier({}, {}, std::span(&barrier, 1));
    }

    //==================================================================================================================
    /// Ends `list`, a Graphics list of `device`, submits it signalling `value` on `done`, and waits for the value.
    void submitAndWait(Device device, CommandList list, Fence done, uint64_t value)
    {
        list.end();
        const std::array<FenceValue, 1> signal{ FenceValue{ done, value } };
        device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, signal);
        REQUIRE(device.wait({ done, value }, UINT64_MAX));
    }

    //==================================================================================================================
    /// Ends `compute`, a Compute list of `device`, submits it signalling `value` on `computed`, and joins it into the frame:
    /// an empty Graphics list waits for that value and signals `value` on `done`, which the call waits for.
    void submitComputeAndJoin(Device device, CommandList compute, Fence computed, Fence done, uint64_t value)
    {
        compute.end();
        const std::array<FenceValue, 1> signal{ FenceValue{ computed, value } };
        device.queue(QueueType::Compute).submit(std::span(&compute, 1), {}, signal);
        CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.join");
        graphics.end();
        const std::array<FenceValue, 1> doneSignal{ FenceValue{ done, value } };
        device.queue(QueueType::Graphics).submit(std::span(&graphics, 1), signal, doneSignal);
        REQUIRE(device.wait({ done, value }, UINT64_MAX));
    }

    //==================================================================================================================
    /// Returns the RGBA8 texel `color` clears an `RGBA8Unorm` texture to.
    std::array<std::byte, 4> rgba8(ClearColor color)
    {
        const auto unorm = [](float value) { return static_cast<std::byte>(std::lround(value * 255.0f)); };
        return { unorm(color.r), unorm(color.g), unorm(color.b), unorm(color.a) };
    }

    //==================================================================================================================
    /// Returns the bytes of the 32-bit float `value`.
    std::array<std::byte, 4> float32(float value)
    {
        return std::bit_cast<std::array<std::byte, 4>>(value);
    }

    //==================================================================================================================
    /// Returns how many texels of `bytes`, tight texels of `texel.size()` bytes, differ from `texel`.
    size_t mismatches(const std::vector<std::byte>& bytes, std::span<const std::byte> texel)
    {
        size_t differing = 0;
        for (size_t offset = 0; offset + texel.size() <= bytes.size(); offset += texel.size())
        {
            if (!std::equal(texel.begin(), texel.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset)))
            {
                ++differing;
            }
        }
        return differing;
    }

    //==================================================================================================================
    /// Checks that native subresource `subresource` of `texture`, in `layout` and no longer in use, is `texels` texels that
    /// all equal `texel`.
    void expectTexels(Device device, Texture texture, uint32_t subresource, D3D12_BARRIER_LAYOUT layout, std::span<const std::byte> texel, size_t texels)
    {
        INFO("subresource " << subresource);
        const std::vector<std::byte> bytes = test::readNativeTexture(DeviceImpl::of(device), payloadOf(device, texture).resource, subresource, layout);
        CHECK(bytes.size() == texels * texel.size());
        CHECK(mismatches(bytes, texel) == 0u);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a render target's clear is two barriers and one clear per mip and layer", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture texture = makeTexture(
        device,
        { .type = TextureType::Tex2DArray,
          .format = Format::RGBA8Unorm,
          .extent = { 8, 8, 1 },
          .mips = 2,
          .layers = 3,
          .usage = TextureUsage::RenderTarget,
          .label = "test.target" }
    );
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.clear");
    record(list, enter(texture));
    const uint64_t calls = recordCalls(list);
    const uint64_t barriers = nativeBarriers(list);
    const uint64_t writes = descriptorWrites(device);
    list.clearTexture(texture, {}, { .color = kFirst });
    // The bracket's two barriers and one ClearRenderTargetView for each of 2 mips of 3 layers.
    CHECK(recordCalls(list) - calls == 8u);
    CHECK(nativeBarriers(list) - barriers == 2u);
    // The render-target views were written at creation; the clear writes no descriptor.
    CHECK(descriptorWrites(device) == writes);
    record(list, toCopySrc(texture));
    submitAndWait(device, list, done, 1);
    const std::array<std::byte, 4> texel = rgba8(kFirst);
    for (uint32_t layer = 0; layer < 3; ++layer)
    {
        expectTexels(device, texture, 0 + layer * 2, D3D12_BARRIER_LAYOUT_COPY_SOURCE, texel, 8 * 8);
        expectTexels(device, texture, 1 + layer * 2, D3D12_BARRIER_LAYOUT_COPY_SOURCE, texel, 4 * 4);
    }
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a 3D render target clears each depth slice of each mip in the range", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture volume = makeTexture(
        device,
        { .type = TextureType::Tex3D,
          .format = Format::RGBA8Unorm,
          .extent = { 8, 8, 4 },
          .mips = 3,
          .usage = TextureUsage::RenderTarget,
          .label = "test.volume" }
    );
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.clear");
    record(list, enter(volume));
    uint64_t calls = recordCalls(list);
    list.clearTexture(volume, {}, { .color = kFirst });
    // Two barriers and the depth slices of every mip: 4 + 2 + 1.
    CHECK(recordCalls(list) - calls == 2u + 7u);
    record(list, rewrite(volume));
    calls = recordCalls(list);
    const uint64_t barriers = nativeBarriers(list);
    list.clearTexture(volume, { .baseMip = 1 }, { .color = kSecond });
    // Mips 1 and 2 alone: 2 + 1 slices.
    CHECK(recordCalls(list) - calls == 2u + 3u);
    CHECK(nativeBarriers(list) - barriers == 2u);
    record(list, toCopySrc(volume));
    submitAndWait(device, list, done, 1);
    expectTexels(device, volume, 0, D3D12_BARRIER_LAYOUT_COPY_SOURCE, rgba8(kFirst), 8 * 8 * 4);
    expectTexels(device, volume, 1, D3D12_BARRIER_LAYOUT_COPY_SOURCE, rgba8(kSecond), 4 * 4 * 2);
    expectTexels(device, volume, 2, D3D12_BARRIER_LAYOUT_COPY_SOURCE, rgba8(kSecond), 2 * 2 * 1);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(volume);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a depth-stencil clear writes depth and stencil of a D32FloatS8 texture", "[d3d12]")
{
    constexpr float kDepth = 0.25f;
    constexpr uint8_t kStencil = 0x5A;
    Device device = makeDevice();
    REQUIRE(device.formatCapabilities(Format::D32FloatS8).depthAttachment);
    REQUIRE(device.formatCapabilities(Format::D32Float).depthAttachment);
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture stencil =
        makeTexture(device, { .format = Format::D32FloatS8, .extent = { 4, 4, 1 }, .mips = 2, .usage = TextureUsage::DepthStencil, .label = "test.stencil" });
    const Texture depth = makeTexture(
        device, { .format = Format::D32Float, .extent = { 4, 4, 1 }, .usage = TextureUsage::DepthStencil | TextureUsage::Sampled, .label = "test.depth" }
    );
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.clear");
    record(list, enter(stencil));
    record(list, enter(depth));
    const uint64_t calls = recordCalls(list);
    const uint64_t barriers = nativeBarriers(list);
    list.clearTexture(stencil, {}, { .depth = kDepth, .stencil = kStencil });
    // Two barriers and one ClearDepthStencilView per mip.
    CHECK(recordCalls(list) - calls == 2u + 2u);
    CHECK(nativeBarriers(list) - barriers == 2u);
    list.clearTexture(depth, {}, { .depth = kDepth });
    CHECK(recordCalls(list) - calls == 4u + 3u);
    submitAndWait(device, list, done, 1);
    // Both planes are back in COPY_DEST and both hold the clear: plane 1 of a 2-mip, 1-layer texture starts at index 2.
    const std::array<std::byte, 4> depthTexel = float32(kDepth);
    const std::array<std::byte, 1> stencilTexel = { std::byte{ kStencil } };
    expectTexels(device, stencil, 0, D3D12_BARRIER_LAYOUT_COPY_DEST, depthTexel, 4 * 4);
    expectTexels(device, stencil, 1, D3D12_BARRIER_LAYOUT_COPY_DEST, depthTexel, 2 * 2);
    expectTexels(device, stencil, 2, D3D12_BARRIER_LAYOUT_COPY_DEST, stencilTexel, 4 * 4);
    expectTexels(device, stencil, 3, D3D12_BARRIER_LAYOUT_COPY_DEST, stencilTexel, 2 * 2);
    expectTexels(device, depth, 0, D3D12_BARRIER_LAYOUT_COPY_DEST, depthTexel, 4 * 4);
    // A later barrier names both planes in CopyDst: the debug layer checks the stencil plane's layout too.
    CommandList leaving = device.acquireCommandList(QueueType::Graphics, "test.leave");
    const std::array<TextureBarrier, 2> out = {
        TextureBarrier{ .texture = stencil,
                        .srcStage = Stage::Copy,
                        .srcAccess = Access::CopyWrite,
                        .dstStage = Stage::DepthStencil,
                        .dstAccess = Access::DepthStencilRead | Access::DepthStencilWrite,
                        .oldLayout = Layout::CopyDst,
                        .newLayout = Layout::DepthStencilWrite },
        toCopySrc(depth),
    };
    leaving.barrier({}, {}, out);
    submitAndWait(device, leaving, done, 2);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(stencil);
    device.destroy(depth);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Storage-only texture clears once per mip through two transient descriptors", "[d3d12]")
{
    Device device = makeDevice();
    REQUIRE(device.formatCapabilities(Format::RGBA8Unorm).storageWrite);
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const std::array<TextureDesc, 4> descs = {
        TextureDesc{ .type = TextureType::Tex1DArray, .format = Format::RGBA8Unorm, .extent = { 16, 1, 1 }, .layers = 3, .usage = TextureUsage::Storage },
        TextureDesc{
            .type = TextureType::Tex2DArray, .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .mips = 2, .layers = 3, .usage = TextureUsage::Storage },
        TextureDesc{
            .type = TextureType::CubeArray, .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .mips = 2, .layers = 12, .usage = TextureUsage::Storage },
        TextureDesc{ .type = TextureType::Tex3D, .format = Format::RGBA8Unorm, .extent = { 8, 8, 4 }, .mips = 2, .usage = TextureUsage::Storage },
    };
    const std::array<std::byte, 4> texel = rgba8(kFirst);
    uint64_t value = 0;
    for (const TextureDesc& desc : descs)
    {
        INFO("texture type " << static_cast<int>(desc.type));
        const Texture texture = makeTexture(device, desc);
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.clear");
        record(list, enter(texture));
        const uint64_t calls = recordCalls(list);
        const uint64_t barriers = nativeBarriers(list);
        const uint64_t writes = descriptorWrites(device);
        list.clearTexture(texture, {}, { .color = kFirst });
        // Two barriers and one ClearUnorderedAccessViewFloat per mip, each through a ring view written twice.
        CHECK(recordCalls(list) - calls == 2u + desc.mips);
        CHECK(nativeBarriers(list) - barriers == 2u);
        CHECK(descriptorWrites(device) - writes == 2u * desc.mips);
        record(list, toCopySrc(texture));
        submitAndWait(device, list, done, ++value);
        const bool volume = desc.type == TextureType::Tex3D;
        for (uint32_t layer = 0; layer < desc.layers; ++layer)
        {
            for (uint32_t mip = 0; mip < desc.mips; ++mip)
            {
                const uint32_t width = std::max(desc.extent.width >> mip, 1u);
                const uint32_t height = std::max(desc.extent.height >> mip, 1u);
                const uint32_t slices = volume ? std::max(desc.extent.depth >> mip, 1u) : 1u;
                expectTexels(device, texture, mip + layer * desc.mips, D3D12_BARRIER_LAYOUT_COPY_SOURCE, texel, size_t{ width } * height * slices);
            }
        }
        device.endFrame();
        device.destroy(texture);
    }
    CHECK(messages.count() == 0u);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Storage-only texture clears on a Compute list", "[d3d12]")
{
    Device device = makeDevice();
    REQUIRE(device.formatCapabilities(Format::R32Float).storageWrite);
    const MessageCollection messages;
    const Fence computed = makeFence(device, "test.computed");
    const Fence done = makeFence(device, "test.done");
    const Texture texture =
        makeTexture(device, { .format = Format::R32Float, .extent = { 8, 8, 1 }, .mips = 2, .usage = TextureUsage::Storage, .label = "test.storage" });
    device.beginFrame();
    CommandList compute = device.acquireCommandList(QueueType::Compute, "test.clear");
    record(compute, enter(texture));
    const uint64_t calls = recordCalls(compute);
    const uint64_t barriers = nativeBarriers(compute);
    const uint64_t writes = descriptorWrites(device);
    compute.clearTexture(texture, {}, { .color = { -2.5f, 0.0f, 0.0f, 0.0f } });
    CHECK(recordCalls(compute) - calls == 2u + 2u);
    CHECK(nativeBarriers(compute) - barriers == 2u);
    CHECK(descriptorWrites(device) - writes == 4u);
    record(compute, toCopySrc(texture));
    submitComputeAndJoin(device, compute, computed, done, 1);
    expectTexels(device, texture, 0, D3D12_BARRIER_LAYOUT_COPY_SOURCE, float32(-2.5f), 8 * 8);
    expectTexels(device, texture, 1, D3D12_BARRIER_LAYOUT_COPY_SOURCE, float32(-2.5f), 4 * 4);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(computed);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a RenderTarget+Storage texture clears through its render-target view on a Graphics list", "[d3d12]")
{
    Device device = makeDevice();
    REQUIRE(device.formatCapabilities(Format::RGBA8Unorm).storageWrite);
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture texture = makeTexture(
        device,
        { .type = TextureType::Tex2DArray,
          .format = Format::RGBA8Unorm,
          .extent = { 8, 8, 1 },
          .mips = 2,
          .layers = 2,
          .usage = TextureUsage::RenderTarget | TextureUsage::Storage,
          .label = "test.both" }
    );
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.clear");
    record(list, enter(texture));
    const uint64_t calls = recordCalls(list);
    const uint64_t writes = descriptorWrites(device);
    list.clearTexture(texture, {}, { .color = kThird });
    // One clear per mip and layer and no transient view: the render-target path, not the unordered-access one.
    CHECK(recordCalls(list) - calls == 2u + 4u);
    CHECK(descriptorWrites(device) == writes);
    record(list, toCopySrc(texture));
    submitAndWait(device, list, done, 1);
    for (uint32_t layer = 0; layer < 2; ++layer)
    {
        expectTexels(device, texture, 0 + layer * 2, D3D12_BARRIER_LAYOUT_COPY_SOURCE, rgba8(kThird), 8 * 8);
        expectTexels(device, texture, 1 + layer * 2, D3D12_BARRIER_LAYOUT_COPY_SOURCE, rgba8(kThird), 4 * 4);
    }
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a multisampled render target and a placed render target clear", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const TextureDesc placedDesc{ .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .mips = 2, .usage = TextureUsage::RenderTarget, .label = "test.placed" };
    const PlacementRequirements requirements = device.placementRequirements(placedDesc);
    Result<Heap> heap = device.createHeap({ .size = requirements.size, .memory = Memory::Gpu, .label = "test.heap" });
    REQUIRE(heap.has_value());
    const Texture placed = makeTexture(device, placedDesc, { *heap, 0 });
    REQUIRE((device.formatCapabilities(Format::RGBA8Unorm).sampleCounts & 4u) != 0);
    const Texture samples = makeTexture(
        device,
        { .type = TextureType::Tex2DArray,
          .format = Format::RGBA8Unorm,
          .extent = { 8, 8, 1 },
          .layers = 2,
          .samples = 4,
          .usage = TextureUsage::RenderTarget,
          .label = "test.samples" }
    );
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.clear");
    record(list, enter(placed));
    uint64_t calls = recordCalls(list);
    list.clearTexture(placed, {}, { .color = kSecond });
    CHECK(recordCalls(list) - calls == 2u + 2u);
    record(list, toCopySrc(placed));
    record(list, enter(samples));
    calls = recordCalls(list);
    list.clearTexture(samples, {}, { .color = kFirst });
    // One clear per layer clears every sample of it.
    CHECK(recordCalls(list) - calls == 2u + 2u);
    record(list, toCopySrc(samples));
    submitAndWait(device, list, done, 1);
    expectTexels(device, placed, 0, D3D12_BARRIER_LAYOUT_COPY_SOURCE, rgba8(kSecond), 8 * 8);
    expectTexels(device, placed, 1, D3D12_BARRIER_LAYOUT_COPY_SOURCE, rgba8(kSecond), 4 * 4);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(samples);
    device.destroy(placed);
    device.destroy(*heap);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a clear writes the value into every subresource of the range and nothing else", "[d3d12]")
{
    constexpr uint32_t kMips = 3;
    constexpr uint32_t kLayers = 3;
    // Mips 1 and 2 of layers 1 and 2, through kAllMips and kAllLayers from non-zero bases; then mip 0 of layer 2 alone.
    constexpr SubresourceRange kUpper = { .baseMip = 1, .mipCount = kAllMips, .baseLayer = 1, .layerCount = kAllLayers };
    constexpr SubresourceRange kSingle = { .baseMip = 0, .mipCount = 1, .baseLayer = 2, .layerCount = 1 };
    Device device = makeDevice();
    REQUIRE(device.formatCapabilities(Format::R32Float).storageWrite);
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture target = makeTexture(
        device,
        { .type = TextureType::Tex2DArray,
          .format = Format::RGBA8Unorm,
          .extent = { 8, 8, 1 },
          .mips = kMips,
          .layers = kLayers,
          .usage = TextureUsage::RenderTarget,
          .label = "test.target" }
    );
    const Texture storage = makeTexture(
        device,
        { .type = TextureType::Tex2DArray,
          .format = Format::R32Float,
          .extent = { 8, 8, 1 },
          .mips = kMips,
          .layers = kLayers,
          .usage = TextureUsage::Storage,
          .label = "test.storage" }
    );
    const std::array<ClearValue, 3> targetValues = { ClearValue{ .color = kFirst }, ClearValue{ .color = kSecond }, ClearValue{ .color = kThird } };
    const std::array<ClearValue, 3> storageValues = { ClearValue{ .color = { 0.5f, 0.0f, 0.0f, 0.0f } },
                                                      ClearValue{ .color = { 1.5f, 0.0f, 0.0f, 0.0f } },
                                                      ClearValue{ .color = { -3.0f, 0.0f, 0.0f, 0.0f } } };
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.clear");
    for (const Texture texture : { target, storage })
    {
        const std::array<ClearValue, 3>& values = texture == target ? targetValues : storageValues;
        record(list, enter(texture));
        list.clearTexture(texture, {}, values[0]);
        record(list, rewrite(texture));
        uint64_t calls = recordCalls(list);
        uint64_t writes = descriptorWrites(device);
        list.clearTexture(texture, kUpper, values[1]);
        CHECK(recordCalls(list) - calls == (texture == target ? 2u + 4u : 2u + 2u));
        CHECK(descriptorWrites(device) - writes == (texture == target ? 0u : 4u));
        record(list, rewrite(texture, kSingle));
        calls = recordCalls(list);
        writes = descriptorWrites(device);
        list.clearTexture(texture, kSingle, values[2]);
        CHECK(recordCalls(list) - calls == 3u);
        CHECK(descriptorWrites(device) - writes == (texture == target ? 0u : 2u));
    }
    submitAndWait(device, list, done, 1);
    // Read in COPY_DEST, where the clears left every subresource.
    for (uint32_t layer = 0; layer < kLayers; ++layer)
    {
        for (uint32_t mip = 0; mip < kMips; ++mip)
        {
            const size_t which = (mip == 0 && layer == 2) ? 2u : (mip >= 1 && layer >= 1) ? 1u : 0u;
            const size_t texels = size_t{ 8u >> mip } * (8u >> mip);
            const uint32_t subresource = mip + layer * kMips;
            expectTexels(device, target, subresource, D3D12_BARRIER_LAYOUT_COPY_DEST, rgba8(targetValues[which].color), texels);
            expectTexels(device, storage, subresource, D3D12_BARRIER_LAYOUT_COPY_DEST, float32(storageValues[which].color.r), texels);
        }
    }
    CommandList leaving = device.acquireCommandList(QueueType::Graphics, "test.leave");
    const std::array<TextureBarrier, 2> out = { toCopySrc(target), toCopySrc(storage) };
    leaving.barrier({}, {}, out);
    submitAndWait(device, leaving, done, 2);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(target);
    device.destroy(storage);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a clear leaves its subresources in CopyDst", "[d3d12]")
{
    constexpr SubresourceRange kRange = { .baseMip = 1, .mipCount = 1, .baseLayer = 1, .layerCount = 1 };
    Device device = makeDevice();
    REQUIRE(device.formatCapabilities(Format::D32FloatS8).depthAttachment);
    REQUIRE(device.formatCapabilities(Format::R32Float).storageWrite);
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const std::array<Texture, 3> textures = {
        makeTexture(
            device,
            { .type = TextureType::Tex2DArray,
              .format = Format::RGBA8Unorm,
              .extent = { 4, 4, 1 },
              .mips = 2,
              .layers = 2,
              .usage = TextureUsage::RenderTarget,
              .label = "test.target" }
        ),
        makeTexture(
            device,
            { .type = TextureType::Tex2DArray,
              .format = Format::D32FloatS8,
              .extent = { 4, 4, 1 },
              .mips = 2,
              .layers = 2,
              .usage = TextureUsage::DepthStencil,
              .label = "test.stencil" }
        ),
        makeTexture(
            device,
            { .type = TextureType::Tex2DArray,
              .format = Format::R32Float,
              .extent = { 4, 4, 1 },
              .mips = 2,
              .layers = 2,
              .usage = TextureUsage::Storage,
              .label = "test.storage" }
        ),
    };
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.clear");
    for (const Texture texture : textures)
    {
        record(list, enter(texture, kRange));
        list.clearTexture(texture, kRange, { .color = kFirst, .depth = 0.5f, .stencil = 7 });
    }
    submitAndWait(device, list, done, 1);
    // A later list takes exactly the cleared subresources out of CopyDst; a layout the clear left behind would fail it.
    CommandList leaving = device.acquireCommandList(QueueType::Graphics, "test.leave");
    const std::array<TextureBarrier, 3> out = { toCopySrc(textures[0], kRange), toCopySrc(textures[1], kRange), toCopySrc(textures[2], kRange) };
    leaving.barrier({}, {}, out);
    submitAndWait(device, leaving, done, 2);
    // Subresource 3 is mip 1 of layer 1; plane 1 of the depth-stencil texture starts at index 4.
    expectTexels(device, textures[0], 3, D3D12_BARRIER_LAYOUT_COPY_SOURCE, rgba8(kFirst), 2 * 2);
    expectTexels(device, textures[1], 3, D3D12_BARRIER_LAYOUT_COPY_SOURCE, float32(0.5f), 2 * 2);
    expectTexels(device, textures[1], 3 + 4, D3D12_BARRIER_LAYOUT_COPY_SOURCE, std::array<std::byte, 1>{ std::byte{ 7 } }, 2 * 2);
    expectTexels(device, textures[2], 3, D3D12_BARRIER_LAYOUT_COPY_SOURCE, float32(kFirst.r), 2 * 2);
    device.endFrame();
    CHECK(messages.count() == 0u);
    for (const Texture texture : textures)
    {
        device.destroy(texture);
    }
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("clears on two threads in one frame take distinct transient views", "[d3d12]")
{
    Device device = makeDevice();
    REQUIRE(device.formatCapabilities(Format::R32Float).storageWrite);
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const std::array<float, 2> values = { 0.125f, 4.0f };
    std::array<Texture, 2> textures = {};
    for (size_t i = 0; i < textures.size(); ++i)
    {
        textures[i] =
            makeTexture(device, { .format = Format::R32Float, .extent = { 8, 8, 1 }, .mips = 2, .usage = TextureUsage::Storage, .label = "test.storage" });
    }
    device.beginFrame();
    const uint64_t writes = descriptorWrites(device);
    std::array<CommandList, 2> lists = {};
    std::array<std::thread, 2> threads;
    for (size_t i = 0; i < threads.size(); ++i)
    {
        // Both lists claim views from the same frame slot's ring while the other records.
        threads[i] = std::thread([&device, &lists, &textures, &values, i] {
            CommandList list = device.acquireCommandList(QueueType::Graphics, "test.thread");
            list.barrier({}, {}, std::array<TextureBarrier, 1>{ enter(textures[i]) });
            for (uint32_t k = 0; k < 4; ++k)
            {
                list.barrier({}, {}, std::array<TextureBarrier, 1>{ rewrite(textures[i]) });
                list.clearTexture(textures[i], {}, { .color = { values[i], 0.0f, 0.0f, 0.0f } });
            }
            list.barrier({}, {}, std::array<TextureBarrier, 1>{ toCopySrc(textures[i]) });
            list.end();
            lists[i] = list;
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    // Two threads, four clears each, two mips each, two descriptors per mip.
    CHECK(descriptorWrites(device) - writes == 2u * 4u * 2u * 2u);
    const std::array<FenceValue, 1> signal{ FenceValue{ done, 1 } };
    device.queue(QueueType::Graphics).submit(lists, {}, signal);
    REQUIRE(device.wait({ done, 1 }, UINT64_MAX));
    // A view one list overwrote in the other's slot would clear one texture with the other's value or not at all.
    for (size_t i = 0; i < textures.size(); ++i)
    {
        expectTexels(device, textures[i], 0, D3D12_BARRIER_LAYOUT_COPY_SOURCE, float32(values[i]), 8 * 8);
        expectTexels(device, textures[i], 1, D3D12_BARRIER_LAYOUT_COPY_SOURCE, float32(values[i]), 4 * 4);
    }
    device.endFrame();
    CHECK(messages.count() == 0u);
    for (const Texture texture : textures)
    {
        device.destroy(texture);
    }
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a clear right after a fill binds no root signature or pipeline", "[d3d12]")
{
    constexpr uint64_t kSize = 256;
    constexpr uint32_t kPattern = 0x04030201u;
    Device device = makeDevice();
    REQUIRE(device.formatCapabilities(Format::R32Float).storageWrite);
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    Result<Buffer> buffer = device.createBuffer({ .size = kSize, .usage = BufferUsage::ShaderWrite, .label = "test.filled" });
    REQUIRE(buffer.has_value());
    const Texture texture = makeTexture(device, { .format = Format::R32Float, .extent = { 8, 8, 1 }, .usage = TextureUsage::Storage, .label = "test.storage" });
    DeviceImpl& impl = DeviceImpl::of(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.clear");
    record(list, enter(texture));
    list.fillBuffer(*buffer, 0, kSize, kPattern);
    const uint64_t calls = recordCalls(list);
    list.clearTexture(texture, {}, { .color = { 2.0f, 0.0f, 0.0f, 0.0f } });
    // The bracket and one clear: the heaps the list set at acquisition are all ClearUnorderedAccessViewFloat needs.
    CHECK(recordCalls(list) - calls == 3u);
    const d3d12::Binding& binding = CommandListImpl::of(list).state.binding;
    CHECK(binding.computeRoot == d3d12::ComputeRoot::Kernel);
    CHECK(binding.computePipeline == impl.kernels.fill());
    record(list, toCopySrc(texture));
    submitAndWait(device, list, done, 1);
    std::vector<std::byte> expected(kSize);
    for (size_t i = 0; i < expected.size(); ++i)
    {
        expected[i] = static_cast<std::byte>(i % 4 + 1);
    }
    CHECK(test::readNativeBuffer(impl, resourceOf(device, *buffer), kSize) == expected);
    expectTexels(device, texture, 0, D3D12_BARRIER_LAYOUT_COPY_SOURCE, float32(2.0f), 8 * 8);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(*buffer);
    device.destroy(done);
    destroyDevice(device);
}
