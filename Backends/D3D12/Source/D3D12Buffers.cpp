//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Buffers.cpp
/// @brief Implements the D3D12 buffer and heap entries, their retire hooks and the frame-arena pages.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Buffers.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Core/Buffers.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/FrameArena.h"
#include "Core/HandlePool.h"
#include "Core/Heaps.h"
#include "Core/Records.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Descriptors.h"
#include "D3D12Device.h"
#include "D3D12Memory.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace rojoRHI {
    namespace {
        /// The label of every frame-arena page.
        constexpr std::string_view kArenaLabel = "rojorhi.frame-arena";

        /// What a buffer is made of natively: its size, memory, whether shaders write it and whether it has raw views, and
        /// the heap and offset it is placed at, or no heap for a committed buffer.
        struct NativeBufferDesc
        {
            uint64_t size = 0;           ///< The size the caller asked for, in bytes.
            Memory memory = Memory::Gpu; ///< The memory class.
            bool writable = false;       ///< Shaders write it: unordered access and a raw UAV.
            bool views = false;          ///< It takes raw views; a Readback buffer takes none.
            ID3D12Heap* heap = nullptr;  ///< The heap it is placed in; null for a committed buffer.
            uint64_t offset = 0;         ///< The byte offset in `heap`.
            std::string_view label;      ///< The resolved label.
        };

        //==============================================================================================================
        /// Returns the resource description of a buffer of `size` bytes: its width rounded up to a multiple of 4 so the raw
        /// views cover every byte, and unordered access when shaders write it.
        D3D12_RESOURCE_DESC1 resourceDesc(uint64_t size, bool writable)
        {
            D3D12_RESOURCE_DESC1 desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = (size + 3) & ~uint64_t{ 3 };
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            return desc;
        }

        //==============================================================================================================
        /// Returns the heap properties of `memory` on `impl`'s device.
        D3D12_HEAP_PROPERTIES heapProperties(const DeviceImpl& impl, Memory memory)
        {
            D3D12_HEAP_PROPERTIES properties = {};
            properties.Type = d3d12::heapType(memory, impl.features.gpuUploadHeap);
            return properties;
        }

        //==============================================================================================================
        /// Returns the bytes and alignment a buffer described by `desc` occupies in a heap of `impl`'s device. The runtime
        /// answers `UINT64_MAX` bytes for a description it refuses, which the core's checks rule out: it is logged and aborts,
        /// in every build.
        PlacementRequirements bufferRequirements(const DeviceImpl& impl, const D3D12_RESOURCE_DESC1& desc)
        {
            const D3D12_RESOURCE_ALLOCATION_INFO info = impl.device()->GetResourceAllocationInfo3(0, 1, &desc, nullptr, nullptr, nullptr);
            if (info.SizeInBytes == UINT64_MAX)
            {
                ROJORHI_LOG_ERROR("ID3D12Device12::GetResourceAllocationInfo3 refused a buffer of {} bytes", desc.Width);
                std::abort();
            }
            return PlacementRequirements{ info.SizeInBytes, info.Alignment };
        }

        //==============================================================================================================
        /// Releases what `payload` holds: its raw views, its mapping and its resource, then the payload itself. The caller has
        /// cleared its directory entry, if it was published.
        void discardPayload(DeviceImpl& impl, d3d12::BufferPayload* payload)
        {
            if (payload->viewCount > 0)
            {
                d3d12::releaseRawViews(impl, payload->viewIndex, payload->viewCount);
            }
            if (payload->host != nullptr)
            {
                payload->resource->Unmap(0, nullptr);
            }
            payload->resource->Release();
            delete payload;
        }

        //==============================================================================================================
        /// Creates the native half of a buffer on `impl`'s device: the committed or placed resource, named `desc.label`, the
        /// mapping of an Upload or Readback buffer, `initial` copied through the mapping or staged for a Gpu buffer, and the raw
        /// views. Returns the payload that owns them, or the error of the failing call with nothing left behind.
        Result<d3d12::BufferPayload*> createPayload(DeviceImpl& impl, const NativeBufferDesc& desc, std::span<const std::byte> initial)
        {
            const D3D12_RESOURCE_DESC1 native = resourceDesc(desc.size, desc.writable);
            d3d12::Com<ID3D12Resource> resource;
            if (desc.heap != nullptr)
            {
                const HRESULT created = impl.device()->CreatePlacedResource2(
                    desc.heap, desc.offset, &native, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr, IID_PPV_ARGS(resource.put())
                );
                if (FAILED(created))
                {
                    const std::string what =
                        std::format("ID3D12Device10::CreatePlacedResource2 of buffer \"{}\" ({} bytes at heap offset {})", desc.label, desc.size, desc.offset);
                    return std::unexpected(d3d12::failure(impl, created, what));
                }
            }
            else
            {
                const D3D12_HEAP_PROPERTIES properties = heapProperties(impl, desc.memory);
                const HRESULT created = impl.device()->CreateCommittedResource3(
                    &properties, D3D12_HEAP_FLAG_NONE, &native, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(resource.put())
                );
                if (FAILED(created))
                {
                    const std::string what = std::format("ID3D12Device10::CreateCommittedResource3 of buffer \"{}\" ({} bytes)", desc.label, desc.size);
                    return std::unexpected(d3d12::failure(impl, created, what));
                }
            }
            d3d12::setName(resource.get(), desc.label);
            std::byte* host = nullptr;
            if (desc.memory != Memory::Gpu)
            {
                // An Upload buffer is only written by the CPU; a Readback buffer may be read anywhere.
                const D3D12_RANGE none = { 0, 0 };
                void* mapped = nullptr;
                const HRESULT result = resource->Map(0, desc.memory == Memory::Upload ? &none : nullptr, &mapped);
                core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
                if (FAILED(result))
                {
                    return std::unexpected(d3d12::failure(impl, result, std::format("ID3D12Resource::Map of buffer \"{}\"", desc.label)));
                }
                host = static_cast<std::byte*>(mapped);
            }
            if (!initial.empty())
            {
                // Readback buffers take no initial data, so it is Upload memory, written through the mapping, or Gpu memory.
                if (desc.memory == Memory::Upload)
                {
                    std::memcpy(host, initial.data(), initial.size());
                }
                else if (Result<void> uploaded = impl.stager.uploadBuffer(impl, resource.get(), initial); !uploaded)
                {
                    return std::unexpected(std::move(uploaded.error()));
                }
            }
            uint32_t viewIndex = 0;
            uint32_t viewCount = 0;
            if (desc.views)
            {
                const Result<uint32_t> views = d3d12::createRawViews(impl, resource.get(), native.Width, desc.writable);
                if (!views)
                {
                    if (host != nullptr)
                    {
                        resource->Unmap(0, nullptr);
                    }
                    return std::unexpected(views.error());
                }
                viewIndex = *views;
                viewCount = desc.writable ? 2u : 1u;
            }
            const D3D12_GPU_VIRTUAL_ADDRESS base = resource->GetGPUVirtualAddress();
            auto* payload = new d3d12::BufferPayload{ resource.detach(), base, desc.size, 0, viewIndex, viewCount, host };
            core::DeviceCounts::add(impl.core.counts().cpuAllocations);
            return payload;
        }

        //==============================================================================================================
        /// Stamps `payload`, which the record of `buffer` now owns, with the handle's bits and publishes it in the heap
        /// directory, so an address of the buffer decodes before the handle is returned. A buffer with no raw view is not
        /// published.
        void publish(DeviceImpl& impl, d3d12::BufferPayload* payload, Buffer buffer)
        {
            payload->bufferBits = buffer.bits;
            if (payload->viewCount > 0)
            {
                impl.directory.publish(payload->viewIndex, payload);
            }
        }

        //==============================================================================================================
        /// Returns the payload of `record`, the record of a live buffer that a checked lookup found.
        const d3d12::BufferPayload& payloadOf(const core::BufferRecord& record)
        {
            return *static_cast<const d3d12::BufferPayload*>(record.native);
        }
    } // namespace

    namespace d3d12 {
        //==============================================================================================================
        void retireBuffer(void* context, core::BufferRecord& record)
        {
            auto* payload = static_cast<BufferPayload*>(record.native);
            if (payload == nullptr)
            {
                return;
            }
            DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
            // The entry goes before its views return to the allocator, so no later buffer's index finds this payload.
            if (payload->viewCount > 0)
            {
                impl.directory.clear(payload->viewIndex);
            }
            discardPayload(impl, payload);
            record.native = nullptr;
        }

        //==============================================================================================================
        void retireHeap(void*, core::HeapRecord& record)
        {
            auto* heap = static_cast<ID3D12Heap*>(record.native);
            if (heap == nullptr)
            {
                return;
            }
            heap->Release();
            record.native = nullptr;
        }

        //==============================================================================================================
        Result<core::ArenaPage> createArenaPage(void* context, uint64_t size)
        {
            core::DeviceCore& core = *static_cast<core::DeviceCore*>(context);
            DeviceImpl& impl = *static_cast<DeviceImpl*>(core.retireHooks().context);
            const BufferDesc desc{ .size = size, .memory = Memory::Upload, .usage = BufferUsage::IndirectArgs, .label = kArenaLabel };
            core::BufferRecord record = core::prepareBuffer(core, desc, {}, {}, {});
            // The page's raw view is taken here, inside the arena's claim, the one lock recording may take (ADR 0020); the view
            // allocator's lock and then the buffer pool's are each released before the next is taken.
            const NativeBufferDesc native{ .size = size, .memory = Memory::Upload, .writable = false, .views = true, .label = record.label };
            Result<BufferPayload*> payload = createPayload(impl, native, {});
            if (!payload)
            {
                return std::unexpected(std::move(payload.error()));
            }
            record.native = *payload;
            Result<Buffer> page = core::addFrameArenaPage(core, std::move(record));
            if (!page)
            {
                discardPayload(impl, *payload);
                return std::unexpected(std::move(page.error()));
            }
            publish(impl, *payload, *page);
            // An address of frame data is the page's raw-view index and an offset, not a GPU address, so the page has none.
            return core::ArenaPage{ .buffer = *page, .host = (*payload)->host, .size = size, .address = 0 };
        }
    } // namespace d3d12

    //==================================================================================================================
    Result<Buffer> Device::createBuffer(const BufferDesc& desc, std::span<const std::byte> initial, Placement placement)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const bool placed = placement.heap.bits != 0;
        const bool writable = (desc.usage & BufferUsage::ShaderWrite) != BufferUsage::None;
        PlacementRequirements requirements;
        if (placed)
        {
            // The runtime never sees a description the core rejects.
            core::checkBufferDesc(impl.core, desc);
            requirements = bufferRequirements(impl, resourceDesc(desc.size, writable));
        }
        core::BufferRecord record = core::prepareBuffer(impl.core, desc, initial, placement, requirements);
        // Sparse never reaches here in a Debug build: the device reports no sparse resources (VU-rojo-buffer-0005).
        const NativeBufferDesc native{
            .size = desc.size,
            .memory = desc.memory,
            .writable = writable,
            .views = desc.memory != Memory::Readback,
            .heap = placed ? static_cast<ID3D12Heap*>(core::checkedHeap(impl.core, placement.heap).native) : nullptr,
            .offset = placed ? placement.offset : 0,
            .label = record.label,
        };
        Result<d3d12::BufferPayload*> payload = createPayload(impl, native, initial);
        if (!payload)
        {
            return std::unexpected(std::move(payload.error()));
        }
        record.native = *payload;
        Result<Buffer> handle = core::addBuffer(impl.core, std::move(record));
        if (!handle)
        {
            discardPayload(impl, *payload);
            return handle;
        }
        // The pool's record owns the payload now; the retire hook frees it.
        publish(impl, *payload, *handle);
        return handle;
    }

    //==================================================================================================================
    Result<Heap> Device::createHeap(const HeapDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::HeapRecord record = core::prepareHeap(desc);
        // A Gpu heap takes multisample textures, which need the larger alignment.
        const uint64_t alignment = desc.memory == Memory::Gpu ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        if (desc.size > UINT64_MAX - (alignment - 1))
        {
            return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("no device memory for {} bytes of heap \"{}\"", desc.size, record.label) });
        }
        D3D12_HEAP_DESC native = {};
        native.SizeInBytes = (desc.size + alignment - 1) / alignment * alignment;
        native.Properties = heapProperties(impl, desc.memory);
        native.Alignment = alignment;
        native.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
        d3d12::Com<ID3D12Heap> heap;
        const HRESULT created = impl.device()->CreateHeap(&native, IID_PPV_ARGS(heap.put()));
        if (FAILED(created))
        {
            const std::string what = std::format("ID3D12Device::CreateHeap of heap \"{}\" ({} bytes)", record.label, native.SizeInBytes);
            return std::unexpected(d3d12::failure(impl, created, what));
        }
        d3d12::setName(heap.get(), record.label);
        record.size = native.SizeInBytes;
        record.native = heap.get();
        const Heap handle = impl.core.pool<Heap>().allocate(std::move(record));
        if (handle.bits == 0)
        {
            // The record was not taken, so the heap's one reference is still `heap`'s and goes with it.
            return std::unexpected(core::tableFull("heap", impl.core.pool<Heap>().capacity()));
        }
        // The record owns the reference now; the heap's retirement releases it.
        (void)heap.detach();
        return handle;
    }

    //==================================================================================================================
    PlacementRequirements Device::placementRequirements(const BufferDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkBufferRequirementsQuery(impl.core, desc);
        return bufferRequirements(impl, resourceDesc(desc.size, (desc.usage & BufferUsage::ShaderWrite) != BufferUsage::None));
    }

    //==================================================================================================================
    Addr Device::address(Buffer buffer, uint64_t offset)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const core::BufferRecord& record = core::checkedBuffer(impl.core, buffer);
        core::checkBufferAddress(record, offset, d3d12::kMaxAddrOffset);
        return d3d12::encodeAddr(payloadOf(record), offset);
    }

    //==================================================================================================================
    std::byte* Device::cpuPointer(Buffer buffer)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const core::BufferRecord& record = core::checkedBuffer(impl.core, buffer);
        core::checkCpuAccess(record);
        return payloadOf(record).host;
    }

    //==================================================================================================================
    void Device::destroy(Buffer buffer)
    {
        core::destroyBuffer(DeviceImpl::of(*this).core, buffer);
    }

    //==================================================================================================================
    void Device::destroy(Heap heap)
    {
        core::destroyHeap(DeviceImpl::of(*this).core, heap);
    }
} // namespace rojoRHI
