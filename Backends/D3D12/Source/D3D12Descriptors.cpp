//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Descriptors.cpp
/// @brief Implements the D3D12 device's shader-visible heaps, the transient ring and the raw views of buffers.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Descriptors.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Base/Log.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Device.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

namespace rojoRHI::d3d12 {
    namespace {
        /// The bytes one raw view reaches: 2^27 four-byte elements (`D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP`).
        constexpr uint64_t kRawViewBytes = 1ull << 29;

        //==============================================================================================================
        /// Returns the raw SRV description of a buffer `width` bytes wide.
        D3D12_SHADER_RESOURCE_VIEW_DESC rawSrv(uint64_t width)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Buffer.FirstElement = 0;
            desc.Buffer.NumElements = static_cast<UINT>(rawViewElements(width));
            desc.Buffer.StructureByteStride = 0;
            desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            return desc;
        }

        //==============================================================================================================
        /// Returns the raw UAV description, with no counter, of a buffer `width` bytes wide.
        D3D12_UNORDERED_ACCESS_VIEW_DESC rawUav(uint64_t width)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.FirstElement = 0;
            desc.Buffer.NumElements = static_cast<UINT>(rawViewElements(width));
            desc.Buffer.StructureByteStride = 0;
            desc.Buffer.CounterOffsetInBytes = 0;
            desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            return desc;
        }

        //==============================================================================================================
        /// Creates a heap of `count` descriptors of `type` on `impl`'s device, shader-visible when `shaderVisible`, named
        /// `label`.
        Result<Com<ID3D12DescriptorHeap>> createHeap(
            DeviceImpl& impl, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shaderVisible, std::string_view label
        )
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = type;
            desc.NumDescriptors = count;
            desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            desc.NodeMask = 0;
            Com<ID3D12DescriptorHeap> heap;
            if (const HRESULT created = impl.device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.put())); FAILED(created))
            {
                return std::unexpected(failure(impl, created, std::format("ID3D12Device::CreateDescriptorHeap for {} of {} descriptors", label, count)));
            }
            setName(heap.get(), label);
            return heap;
        }

        //==============================================================================================================
        /// Returns the view-heap size `device` supports: the `D3D12_FEATURE_D3D12_OPTIONS19` maximum capped at `kMaxViewHeap`,
        /// or `kMinViewHeap` when the device does not report that feature.
        uint32_t viewHeapSize(ID3D12Device12* device)
        {
            D3D12_FEATURE_DATA_D3D12_OPTIONS19 options = {};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &options, static_cast<UINT>(sizeof(options)))))
            {
                return kMinViewHeap;
            }
            return std::min(options.MaxViewDescriptorHeapSize, kMaxViewHeap);
        }
    } // namespace

    //==================================================================================================================
    Result<void> DescriptorHeaps::create(DeviceImpl& impl)
    {
        const uint32_t capacity = viewHeapSize(impl.device());
        if (capacity < kMinViewHeap)
        {
            return std::unexpected(
                Error{ ErrorCategory::Unsupported, std::format("the D3D12 hardware floor is not met: maxViews is {}, below {}", capacity, kMinViewHeap) }
            );
        }
        Result<Com<ID3D12DescriptorHeap>> views = createHeap(impl, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, capacity, true, "rojorhi.views");
        if (!views)
        {
            return std::unexpected(std::move(views.error()));
        }
        Result<Com<ID3D12DescriptorHeap>> samplers = createHeap(impl, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeap, true, "rojorhi.samplers");
        if (!samplers)
        {
            return std::unexpected(std::move(samplers.error()));
        }
        const uint32_t blocks = impl.core.framesInFlight();
        ROJORHI_INTERNAL_ASSERT(blocks <= m_ringClaims.size(), "more frame slots than transient ring blocks");
        const uint32_t ringSize = blocks * kTransientPerSlot;
        Result<Com<ID3D12DescriptorHeap>> twins = createHeap(impl, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ringSize, false, "rojorhi.transient");
        if (!twins)
        {
            return std::unexpected(std::move(twins.error()));
        }
        m_views = std::move(*views);
        m_samplers = std::move(*samplers);
        m_twins = std::move(*twins);
        m_viewCpu = m_views->GetCPUDescriptorHandleForHeapStart();
        m_viewGpu = m_views->GetGPUDescriptorHandleForHeapStart();
        m_samplerCpu = m_samplers->GetCPUDescriptorHandleForHeapStart();
        m_twinCpu = m_twins->GetCPUDescriptorHandleForHeapStart();
        m_viewIncrement = impl.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_samplerIncrement = impl.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        m_viewCapacity = capacity;
        m_ringBlocks = blocks;

        // Slots 0 and 1 are the first slots a fresh allocator hands out, and they are never released: the core's teardown
        // drains them.
        for (uint32_t held = 0; held < kHeldViewSlots; ++held)
        {
            [[maybe_unused]] const std::optional<uint32_t> reserved = impl.core.views().allocate(capacity, 0);
            ROJORHI_INTERNAL_ASSERT(reserved == held, "the view heap's held slots are not its first slots");
        }
        // A null raw SRV and a null raw UAV, described as a four-byte buffer, so a load through the zero Addr reads zeros and
        // a store through it is discarded.
        const D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = rawSrv(4);
        impl.device()->CreateShaderResourceView(nullptr, &nullSrv, viewCpu(0));
        core::DeviceCounts::add(impl.core.counts().descriptorWrites);
        const D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav = rawUav(4);
        impl.device()->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, viewCpu(1));
        core::DeviceCounts::add(impl.core.counts().descriptorWrites);

        const std::optional<uint32_t> ring = impl.core.views().allocateRange(capacity, ringSize, 0);
        if (!ring)
        {
            return std::unexpected(
                Error{ ErrorCategory::OutOfMemory,
                       std::format("maxViews: the view heap of {} descriptors has no room for the transient ring of {}", capacity, ringSize) }
            );
        }
        m_ringBase = *ring;
        return {};
    }

    //==================================================================================================================
    uint32_t DescriptorHeaps::viewCapacity() const
    {
        return m_viewCapacity;
    }

    //==================================================================================================================
    uint32_t DescriptorHeaps::samplerCapacity() const
    {
        return kSamplerHeap;
    }

    //==================================================================================================================
    ID3D12DescriptorHeap* DescriptorHeaps::views() const
    {
        return m_views.get();
    }

    //==================================================================================================================
    ID3D12DescriptorHeap* DescriptorHeaps::samplers() const
    {
        return m_samplers.get();
    }

    //==================================================================================================================
    D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeaps::viewCpu(uint32_t index) const
    {
        ROJORHI_INTERNAL_ASSERT(index < m_viewCapacity, "a view-heap index past the heap");
        return D3D12_CPU_DESCRIPTOR_HANDLE{ m_viewCpu.ptr + static_cast<SIZE_T>(index) * m_viewIncrement };
    }

    //==================================================================================================================
    D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeaps::viewGpu(uint32_t index) const
    {
        ROJORHI_INTERNAL_ASSERT(index < m_viewCapacity, "a view-heap index past the heap");
        return D3D12_GPU_DESCRIPTOR_HANDLE{ m_viewGpu.ptr + static_cast<UINT64>(index) * m_viewIncrement };
    }

    //==================================================================================================================
    D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeaps::samplerCpu(uint32_t index) const
    {
        ROJORHI_INTERNAL_ASSERT(index < kSamplerHeap, "a sampler-heap index past the heap");
        return D3D12_CPU_DESCRIPTOR_HANDLE{ m_samplerCpu.ptr + static_cast<SIZE_T>(index) * m_samplerIncrement };
    }

    //==================================================================================================================
    TransientSlot DescriptorHeaps::claimTransient(uint32_t frameSlot)
    {
        ROJORHI_INTERNAL_ASSERT(frameSlot < m_ringBlocks, "a transient claim for a frame slot the device does not have");
        const uint32_t claimed = m_ringClaims[frameSlot].fetch_add(1, std::memory_order_relaxed);
        if (claimed >= kTransientPerSlot)
        {
            ROJORHI_LOG_ERROR("frame slot {} claimed transient descriptor {}: at most 1,024 storage-texture clears per frame slot", frameSlot, claimed + 1);
            std::abort();
        }
        const uint32_t offset = frameSlot * kTransientPerSlot + claimed;
        TransientSlot slot;
        slot.index = m_ringBase + offset;
        slot.gpu = viewGpu(slot.index);
        slot.cpu = viewCpu(slot.index);
        slot.cpuTwin = D3D12_CPU_DESCRIPTOR_HANDLE{ m_twinCpu.ptr + static_cast<SIZE_T>(offset) * m_viewIncrement };
        return slot;
    }

    //==================================================================================================================
    void DescriptorHeaps::resetTransient(uint32_t frameSlot)
    {
        ROJORHI_INTERNAL_ASSERT(frameSlot < m_ringBlocks, "a transient reset for a frame slot the device does not have");
        m_ringClaims[frameSlot].store(0, std::memory_order_relaxed);
    }

    //==================================================================================================================
    Result<uint32_t> createRawViews(DeviceImpl& impl, ID3D12Resource* resource, uint64_t width, bool writable)
    {
        ROJORHI_INTERNAL_ASSERT(resource != nullptr, "raw views of no resource");
        ROJORHI_INTERNAL_ASSERT(width >= 4 && width % 4 == 0, "a raw-view width that is not a positive multiple of 4");
        const uint32_t count = writable ? 2 : 1;
        const uint32_t capacity = impl.descriptors.viewCapacity();
        const std::optional<uint32_t> base = impl.core.views().allocateRange(capacity, count, 0);
        if (!base)
        {
            return std::unexpected(
                Error{ ErrorCategory::OutOfMemory,
                       std::format("maxViews: the view heap of {} descriptors has no {} adjacent free slots for raw views", capacity, count) }
            );
        }
        const D3D12_SHADER_RESOURCE_VIEW_DESC srv = rawSrv(width);
        impl.device()->CreateShaderResourceView(resource, &srv, impl.descriptors.viewCpu(*base));
        core::DeviceCounts::add(impl.core.counts().descriptorWrites);
        if (writable)
        {
            const D3D12_UNORDERED_ACCESS_VIEW_DESC uav = rawUav(width);
            impl.device()->CreateUnorderedAccessView(resource, nullptr, &uav, impl.descriptors.viewCpu(*base + 1));
            core::DeviceCounts::add(impl.core.counts().descriptorWrites);
        }
        return *base;
    }

    //==================================================================================================================
    uint64_t rawViewElements(uint64_t width)
    {
        return std::min(width, kRawViewBytes) / 4;
    }

    //==================================================================================================================
    void releaseRawViews(DeviceImpl& impl, uint32_t base, uint32_t count)
    {
        // The core's teardown passes buffers to their hooks before it drains the view allocator, so the range is held then
        // too.
        [[maybe_unused]] const bool released = impl.core.views().releaseRange(base, count, [&] { return impl.core.releaseFrame(); }).has_value();
        ROJORHI_INTERNAL_ASSERT(released, "raw views released that the view allocator does not hold as one range");
    }
} // namespace rojoRHI::d3d12
