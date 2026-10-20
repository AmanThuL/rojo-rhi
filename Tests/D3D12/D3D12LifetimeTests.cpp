//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12LifetimeTests.cpp
/// @brief Tests that a device releases every native object it created and that a removed device aborts the entries that read it.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "D3D12Device.h"
#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>

using namespace rojoRHI;

namespace {
    /// Returns `value` rounded up to a multiple of `alignment`.
    uint64_t alignUp(uint64_t value, uint64_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    //==================================================================================================================
    /// Drops the messages the runtime reports for a removal the caller asked for, so the layer does not abort before the entry
    /// under test reports it. Does nothing when the device has no debug layer.
    void dropRemovalMessages(DeviceImpl& impl)
    {
        d3d12::Com<ID3D12InfoQueue> queue;
        if (FAILED(impl.device()->QueryInterface(IID_PPV_ARGS(queue.put()))))
        {
            return;
        }
        D3D12_MESSAGE_ID dropped[] = { D3D12_MESSAGE_ID_DEVICE_REMOVAL_PROCESS_AT_FAULT,
                                       D3D12_MESSAGE_ID_DEVICE_REMOVAL_PROCESS_POSSIBLY_AT_FAULT,
                                       D3D12_MESSAGE_ID_DEVICE_REMOVAL_PROCESS_NOT_AT_FAULT };
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumIDs = static_cast<UINT>(std::size(dropped));
        filter.DenyList.pIDList = dropped;
        REQUIRE(SUCCEEDED(queue->AddStorageFilterEntries(&filter)));
    }
} // namespace

//======================================================================================================================
TEST_CASE("completedValue on a removed device reports removal and aborts", "[d3d12][death]")
{
    ROJORHI_EXPECT_ABORT("removed", [] {
        Result<Device> created = createDevice();
        REQUIRE(created.has_value());
        Result<Fence> fence = created->createFence(0, "test.fence");
        REQUIRE(fence.has_value());
        const std::array<FenceValue, 1> signal{ FenceValue{ *fence, 1 } };
        created->queue(QueueType::Graphics).submit({}, {}, signal);
        DeviceImpl& impl = DeviceImpl::of(*created);
        dropRemovalMessages(impl);
        impl.device()->RemoveDevice();
        (void)created->completedValue(*fence);
    });
}

//======================================================================================================================
TEST_CASE("a device with every resource type releases every native object at destruction", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    Device device = *created;

    const std::array<std::byte, 64> bytes{};
    const Buffer gpu = *device.createBuffer({ .size = bytes.size(), .memory = Memory::Gpu, .label = "test.gpu" }, bytes);
    const Buffer upload = *device.createBuffer({ .size = 256, .memory = Memory::Upload, .label = "test.upload" }, bytes);
    const Buffer readback = *device.createBuffer({ .size = 256, .memory = Memory::Readback, .label = "test.readback" });

    // One heap holds a buffer and a texture, the texture at its own alignment after the buffer.
    const BufferDesc placedBuffer{ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.placed.buffer" };
    const TextureDesc placedTexture{ .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::Sampled, .label = "test.placed.texture" };
    const PlacementRequirements bufferRequirements = device.placementRequirements(placedBuffer);
    const PlacementRequirements textureRequirements = device.placementRequirements(placedTexture);
    const uint64_t textureOffset = alignUp(bufferRequirements.size, textureRequirements.alignment);
    const Heap heap = *device.createHeap({ .size = textureOffset + textureRequirements.size, .memory = Memory::Gpu, .label = "test.heap" });
    const Buffer placedInHeap = *device.createBuffer(placedBuffer, {}, { heap, 0 });
    const Texture textureInHeap = *device.createTexture(placedTexture, {}, { heap, textureOffset });

    // A texture with initial data, and a render target.
    const std::array<std::byte, 64> texels{};
    const TextureData level{ .bytes = texels, .bytesPerRow = 16 };
    const Texture textured = *device.createTexture(
        { .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled, .label = "test.textured" }, std::span(&level, 1)
    );
    const Texture target = *device.createTexture(
        { .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .usage = TextureUsage::RenderTarget | TextureUsage::Sampled, .label = "test.target" }
    );

    const ViewIndex view = *device.createView(textured, {});
    const SamplerIndex sampler = *device.createSampler({ .label = "test.sampler" });
    const IndexRange range = *device.reserveViews(4);
    device.writeView(range, 1, textured, {});
    const Fence fence = *device.createFence(0, "test.fence");

    // One frame uploads frame data in a list and submits it.
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    (void)list.uploadFrameData(bytes);
    list.end();
    const std::array<CommandList, 1> lists{ list };
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    device.queue(QueueType::Graphics).submit(lists, {}, signal);
    device.endFrame();
    CHECK(device.wait({ fence, 1 }, UINT64_MAX));

    // Half of the objects go before the device does; the device releases the rest. The view and the range slot that hold
    // views of the textured texture go before it (VU-rojo-texture-0028).
    device.destroy(gpu);
    device.destroy(readback);
    device.destroy(placedInHeap);
    device.destroy(view);
    device.release(range);
    device.destroy(textured);
    device.destroy(fence);
    (void)upload;
    (void)textureInHeap;
    (void)target;
    (void)sampler;
    destroyDevice(device);
}
