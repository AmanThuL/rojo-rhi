//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12DescriptorTests.cpp
/// @brief Tests the D3D12 device's descriptor heaps: their sizes, the held slot 0, the transient ring and raw buffer views.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/SlotAllocator.h"
#include "D3D12Common.h"
#include "D3D12Descriptors.h"
#include "D3D12Device.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <d3d12sdklayers.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <optional>
#include <string>
#include <vector>

using namespace rojoRHI;

namespace {
    /// True when the build selects the WARP adapter, whose view-heap maximum is known.
#if defined(ROJORHI_D3D12_ADAPTER_WARP)
    constexpr bool kWarpAdapter = true;
#else
    constexpr bool kWarpAdapter = false;
#endif

    //==================================================================================================================
    /// Returns the view-heap size a device with `framesInFlight` frame slots should have: the `OPTIONS19` maximum capped at
    /// 2^21, else 1,000,000 more than the held slots and the transient ring.
    uint32_t expectedViewHeap(ID3D12Device12* device, uint32_t framesInFlight)
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS19 options = {};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &options, static_cast<UINT>(sizeof(options)))))
        {
            return 1'000'000 + d3d12::kHeldViewSlots + framesInFlight * d3d12::kTransientPerSlot;
        }
        return std::min(options.MaxViewDescriptorHeapSize, 1u << 21);
    }

    //==================================================================================================================
    /// Creates a committed `DEFAULT` buffer of `width` bytes that allows unordered access, as a buffer with raw views has.
    d3d12::Com<ID3D12Resource> nativeBuffer(ID3D12Device12* device, uint64_t width)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = width;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d3d12::Com<ID3D12Resource> resource;
        const HRESULT created =
            device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(resource.put()));
        INFO("CreateCommittedResource of " << width << " bytes returned 0x" << std::hex << d3d12::hresultBits(created));
        REQUIRE(SUCCEEDED(created));
        return resource;
    }

    /// Counts the debug layer's messages at warning severity or above stored on a device after it is made. With the layer
    /// off the device has no info queue and the count is always zero. Reset it before the device is destroyed: the queue
    /// holds a reference to the device.
    class MessageWatch
    {
      public:
        //==============================================================================================================
        /// Starts counting the messages `device` stores from now on.
        explicit MessageWatch(ID3D12Device12* device)
        {
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(m_queue.put()))))
            {
                m_first = m_queue->GetNumStoredMessages();
            }
        }

        //==============================================================================================================
        /// Returns the messages at warning severity or above stored since the watch started, and lists them for the log.
        uint64_t warnings()
        {
            if (!m_queue)
            {
                return 0;
            }
            uint64_t count = 0;
            const uint64_t stored = m_queue->GetNumStoredMessages();
            for (uint64_t index = m_first; index < stored; ++index)
            {
                SIZE_T length = 0;
                if (FAILED(m_queue->GetMessage(index, nullptr, &length)) || length == 0)
                {
                    continue;
                }
                std::vector<std::byte> bytes(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                if (SUCCEEDED(m_queue->GetMessage(index, message, &length)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                {
                    UNSCOPED_INFO("debug layer: " << std::string(message->pDescription, message->DescriptionByteLength));
                    ++count;
                }
            }
            return count;
        }

        //==============================================================================================================
        /// Releases the info queue, and with it the queue's reference to the device.
        void reset()
        {
            m_queue.reset();
        }

      private:
        d3d12::Com<ID3D12InfoQueue> m_queue;
        uint64_t m_first = 0;
    };

    //==================================================================================================================
    /// Runs one frame on `device`, with no work in it.
    void emptyFrame(Device device)
    {
        device.beginFrame();
        device.endFrame();
    }
} // namespace

//======================================================================================================================
TEST_CASE("the view heap is shader-visible and holds H descriptors", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    const uint32_t capacity = impl.descriptors.viewCapacity();
    CHECK(capacity == expectedViewHeap(impl.device(), impl.core.framesInFlight()));
    CHECK(capacity >= 1'000'000u);
    if constexpr (kWarpAdapter)
    {
        CHECK(capacity == 2'097'152u);
    }
    REQUIRE(impl.descriptors.views() != nullptr);
    const D3D12_DESCRIPTOR_HEAP_DESC desc = impl.descriptors.views()->GetDesc();
    CHECK(desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CHECK(desc.Flags == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    CHECK(desc.NumDescriptors == capacity);
    const uint32_t reserved = d3d12::kHeldViewSlots + impl.core.framesInFlight() * d3d12::kTransientPerSlot;
    CHECK(impl.descriptors.viewSlots() == capacity - reserved);
    CHECK(created->capabilities().maxViews == capacity - reserved);
    CHECK(created->capabilities().maxViews >= 1'000'000u);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the sampler heap is shader-visible and holds 2,048 samplers", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    CHECK(impl.descriptors.samplerCapacity() == 2'048u);
    REQUIRE(impl.descriptors.samplers() != nullptr);
    const D3D12_DESCRIPTOR_HEAP_DESC desc = impl.descriptors.samplers()->GetDesc();
    CHECK(desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    CHECK(desc.Flags == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    CHECK(desc.NumDescriptors == 2'048u);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("slots 0 and 1 are held for the device's life and count as no caller's view", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    core::DeviceCore& core = DeviceImpl::of(*created).core;
    const uint32_t slots = DeviceImpl::of(*created).descriptors.viewSlots();
    CHECK(core.views().state(0) == core::SlotState::Reserved);
    CHECK(core.views().state(1) == core::SlotState::Reserved);
    CHECK(core.views().assignedCount() == 0u);
    for (uint32_t frame = 0; frame <= core.framesInFlight(); ++frame)
    {
        emptyFrame(*created);
    }
    CHECK(core.views().state(0) == core::SlotState::Reserved);
    CHECK(core.views().state(1) == core::SlotState::Reserved);
    const std::optional<uint32_t> slot = core.views().allocate(slots, 7);
    REQUIRE(slot.has_value());
    CHECK(*slot >= d3d12::kHeldViewSlots + core.framesInFlight() * d3d12::kTransientPerSlot);
    CHECK(*slot < DeviceImpl::of(*created).descriptors.viewCapacity());
    CHECK(core.views().release(*slot, [&] { return core.releaseFrame(); }).has_value());
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the ring is the framesInFlight x 1,024 reserved slots after slot 1", "[d3d12]")
{
    const uint32_t framesInFlight = GENERATE(2u, 3u);
    DeviceDesc desc;
    desc.framesInFlight = framesInFlight;
    Result<Device> created = createDevice(desc);
    REQUIRE(created.has_value());
    INFO("framesInFlight " << framesInFlight);
    core::SlotAllocator& views = DeviceImpl::of(*created).core.views();
    const uint32_t end = d3d12::kHeldViewSlots + framesInFlight * 1'024;
    CHECK(views.state(d3d12::kHeldViewSlots) == core::SlotState::Reserved);
    CHECK(views.state(end - 1) == core::SlotState::Reserved);
    CHECK(views.state(end) == core::SlotState::Free);
    const d3d12::TransientSlot first = DeviceImpl::of(*created).descriptors.claimTransient(0);
    CHECK(first.index == d3d12::kHeldViewSlots);
    const d3d12::TransientSlot last = DeviceImpl::of(*created).descriptors.claimTransient(framesInFlight - 1);
    CHECK(last.index == end - 1'024);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("transient claims are contiguous within a frame slot and restart after reset", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    d3d12::DescriptorHeaps& heaps = DeviceImpl::of(*created).descriptors;
    const d3d12::TransientSlot first = heaps.claimTransient(0);
    const d3d12::TransientSlot second = heaps.claimTransient(0);
    CHECK(second.index == first.index + 1);
    CHECK(first.cpu.ptr == heaps.viewCpu(first.index).ptr);
    CHECK(first.gpu.ptr == heaps.viewGpu(first.index).ptr);
    CHECK(first.cpuTwin.ptr != 0u);
    CHECK(second.cpuTwin.ptr > first.cpuTwin.ptr);
    const d3d12::TransientSlot other = heaps.claimTransient(1);
    CHECK(other.index == first.index + 1'024);
    heaps.resetTransient(0);
    const d3d12::TransientSlot again = heaps.claimTransient(0);
    CHECK(again.index == first.index);
    CHECK(again.cpuTwin.ptr == first.cpuTwin.ptr);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a writable raw view pair is adjacent and a read-only one takes one slot", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    MessageWatch messages(impl.device());
    {
        d3d12::Com<ID3D12Resource> buffer = nativeBuffer(impl.device(), 256);
        const uint64_t before = created->counters().descriptorWrites;
        const Result<uint32_t> pair = d3d12::createRawViews(impl, buffer.get(), 256, true);
        REQUIRE(pair.has_value());
        CHECK(*pair >= d3d12::kHeldViewSlots + impl.core.framesInFlight() * d3d12::kTransientPerSlot);
        CHECK(impl.core.views().holdsRange(*pair, 2));
        CHECK(created->counters().descriptorWrites == before + 2);
        const Result<uint32_t> single = d3d12::createRawViews(impl, buffer.get(), 256, false);
        REQUIRE(single.has_value());
        CHECK(impl.core.views().holdsRange(*single, 1));
        CHECK(created->counters().descriptorWrites == before + 3);
        d3d12::releaseRawViews(impl, *pair, 2);
        d3d12::releaseRawViews(impl, *single, 1);
    }
    CHECK(messages.warnings() == 0u);
    messages.reset();
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a raw view of a buffer above 512 MiB stops at 2^27 elements", "[d3d12]")
{
    CHECK(d3d12::rawViewElements((1ull << 29) + 4) == (1ull << 27));
    CHECK(d3d12::rawViewElements(1ull << 29) == (1ull << 27));
    CHECK(d3d12::rawViewElements(4) == 1u);
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    MessageWatch messages(impl.device());
    {
        const uint64_t width = (1ull << 29) + 4;
        d3d12::Com<ID3D12Resource> buffer = nativeBuffer(impl.device(), width);
        const Result<uint32_t> views = d3d12::createRawViews(impl, buffer.get(), width, true);
        REQUIRE(views.has_value());
        d3d12::releaseRawViews(impl, *views, 2);
    }
    CHECK(messages.warnings() == 0u);
    messages.reset();
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("released raw views are not reused before their frame retires", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    {
        d3d12::Com<ID3D12Resource> buffer = nativeBuffer(impl.device(), 256);
        created->beginFrame();
        const Result<uint32_t> released = d3d12::createRawViews(impl, buffer.get(), 256, true);
        REQUIRE(released.has_value());
        d3d12::releaseRawViews(impl, *released, 2);
        CHECK(impl.core.views().state(*released) == core::SlotState::Released);
        CHECK(impl.core.views().state(*released + 1) == core::SlotState::Released);
        const Result<uint32_t> next = d3d12::createRawViews(impl, buffer.get(), 256, true);
        REQUIRE(next.has_value());
        CHECK(*next != *released);
        CHECK(*next != *released + 1);
        CHECK(*next + 1 != *released);
        d3d12::releaseRawViews(impl, *next, 2);
        created->endFrame();
        for (uint32_t frame = 1; frame < impl.core.framesInFlight(); ++frame)
        {
            emptyFrame(*created);
            CHECK(impl.core.views().state(*released) == core::SlotState::Released);
        }
        emptyFrame(*created);
        CHECK(impl.core.views().state(*released) == core::SlotState::Free);
        CHECK(impl.core.views().state(*released + 1) == core::SlotState::Free);
    }
    destroyDevice(*created);
}
