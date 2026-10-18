//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Buffers.cpp
/// @brief Implements the buffer and heap entries of the Metal 4 backend and their retire hooks.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Buffers.h"

#include <rojoRHI/RHI.h>

#include "Core/Buffers.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Heaps.h"
#include "Core/Records.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Memory.h"

#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <utility>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Returns the bytes and alignment a buffer of `desc` occupies in a heap of its memory class on `device`.
        PlacementRequirements placedRequirements(MTL::Device* device, const BufferDesc& desc)
        {
            const MTL::SizeAndAlign answer = device->heapBufferSizeAndAlign(desc.size, metal4::placedOptions(desc.memory));
            return PlacementRequirements{ answer.size, answer.align };
        }

        //==============================================================================================================
        /// Undoes a buffer creation that failed after `buffer` was made: it leaves the device residency set unless it is
        /// placed, and is released. The caller holds an autorelease pool.
        void discardBuffer(DeviceImpl& impl, MTL::Buffer* buffer, bool placed)
        {
            if (!placed)
            {
                impl.residency.remove(buffer);
            }
            buffer->release();
        }
    } // namespace

    namespace metal4 {
        //==============================================================================================================
        void retireBuffer(void* context, core::BufferRecord& record)
        {
            auto* buffer = static_cast<MTL::Buffer*>(record.native);
            if (buffer == nullptr)
            {
                return;
            }
            DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
            const AutoreleasePool pool;
#ifndef NDEBUG
            impl.addresses.erase(buffer->gpuAddress(), buffer);
#endif
            // A placed buffer never joined the set: its heap did.
            if (record.heap.bits == 0)
            {
                impl.residency.remove(buffer);
            }
            buffer->release();
            record.native = nullptr;
        }

        //==============================================================================================================
        void retireHeap(void* context, core::HeapRecord& record)
        {
            auto* heap = static_cast<MTL::Heap*>(record.native);
            if (heap == nullptr)
            {
                return;
            }
            DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
            const AutoreleasePool pool;
            impl.residency.remove(heap);
            heap->release();
            record.native = nullptr;
        }
    } // namespace metal4

    //==================================================================================================================
    Result<Buffer> Device::createBuffer(const BufferDesc& desc, std::span<const std::byte> initial, Placement placement)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const bool placed = placement.heap.bits != 0;
        const metal4::AutoreleasePool pool;
        PlacementRequirements requirements;
        if (placed)
        {
            // Metal never sees a descriptor the core rejects: a size query of zero bytes could abort inside Metal before the
            // violation is reported.
            core::checkBufferDesc(impl.core, desc);
            requirements = placedRequirements(impl.device, desc);
        }
        core::BufferRecord record = core::prepareBuffer(impl.core, desc, initial, placement, requirements);
        MTL::Buffer* buffer = nullptr;
        if (placed)
        {
            auto* heap = static_cast<MTL::Heap*>(core::checkedHeap(impl.core, placement.heap).native);
            buffer = heap->newBuffer(desc.size, metal4::placedOptions(desc.memory), placement.offset);
        }
        else
        {
            buffer = impl.device->newBuffer(desc.size, metal4::resourceOptions(desc.memory));
        }
        if (buffer == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("no device memory for {} bytes of buffer \"{}\"", desc.size, record.label) });
        }
        buffer->setLabel(metal4::makeString(record.label));
        if (!placed)
        {
            impl.residency.add(buffer);
        }
        if (!initial.empty())
        {
            // Readback and Sparse buffers take no initial data, so it is Upload memory, written through the CPU, or Gpu memory.
            if (desc.memory == Memory::Upload)
            {
                std::memcpy(buffer->contents(), initial.data(), initial.size());
            }
            else if (Result<void> uploaded = impl.stager.uploadBuffer(impl, initial, buffer); !uploaded)
            {
                discardBuffer(impl, buffer, placed);
                return std::unexpected(std::move(uploaded.error()));
            }
        }
        record.native = buffer;
        Result<Buffer> handle = core::addBuffer(impl.core, std::move(record));
        if (!handle)
        {
            discardBuffer(impl, buffer, placed);
            return handle;
        }
#ifndef NDEBUG
        // Only Debug checks decode addresses, so only Debug builds keep the address map.
        impl.addresses.insert(buffer->gpuAddress(), desc.size, core::HandlePool<Buffer, core::BufferRecord>::slotIndex(*handle), buffer);
#endif
        return handle;
    }

    //==================================================================================================================
    Result<Heap> Device::createHeap(const HeapDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::HeapRecord record = core::prepareHeap(desc);
        const metal4::AutoreleasePool pool;
        MTL::HeapDescriptor* descriptor = MTL::HeapDescriptor::alloc()->init();
        descriptor->setType(MTL::HeapTypePlacement);
        descriptor->setSize(desc.size);
        descriptor->setStorageMode(metal4::storageMode(desc.memory));
        descriptor->setCpuCacheMode(MTL::CPUCacheModeDefaultCache);
        descriptor->setHazardTrackingMode(MTL::HazardTrackingModeUntracked);
        MTL::Heap* heap = impl.device->newHeap(descriptor);
        descriptor->release();
        if (heap == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("no device memory for {} bytes of heap \"{}\"", desc.size, record.label) });
        }
        record.size = heap->size();
        heap->setLabel(metal4::makeString(record.label));
        impl.residency.add(heap);
        record.native = heap;
        const Heap handle = impl.core.pool<Heap>().allocate(std::move(record));
        if (handle.bits == 0)
        {
            impl.residency.remove(heap);
            heap->release();
            return std::unexpected(core::tableFull("heap", impl.core.pool<Heap>().capacity()));
        }
        return handle;
    }

    //==================================================================================================================
    PlacementRequirements Device::placementRequirements(const BufferDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkBufferRequirementsQuery(impl.core, desc);
        return placedRequirements(impl.device, desc);
    }

    //==================================================================================================================
    Addr Device::address(Buffer buffer, uint64_t offset)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const core::BufferRecord& record = core::checkedBuffer(impl.core, buffer);
        core::checkBufferAddress(record, offset, impl.core.capabilities().limits.maxAddrOffset);
        const uint64_t value = static_cast<MTL::Buffer*>(record.native)->gpuAddress() + offset;
        return Addr{ static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32) };
    }

    //==================================================================================================================
    std::byte* Device::cpuPointer(Buffer buffer)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const core::BufferRecord& record = core::checkedBuffer(impl.core, buffer);
        core::checkCpuAccess(record);
        return static_cast<std::byte*>(static_cast<MTL::Buffer*>(record.native)->contents());
    }

    //==================================================================================================================
    void Device::destroy(Buffer buffer)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
#ifndef NDEBUG
        // Only Debug checks decode addresses. The payload is read while the handle still holds its slot: once destroyed, a
        // frame may retire the slot and release the buffer. A bad handle is reported by the destroy before anything is marked.
        const auto* native = static_cast<const MTL::Buffer*>(impl.core.pool<Buffer>().native(buffer));
        const uint64_t base = native != nullptr ? native->gpuAddress() : 0;
#endif
        core::destroyBuffer(impl.core, buffer);
#ifndef NDEBUG
        if (native != nullptr)
        {
            impl.addresses.markDestroyed(base, native);
        }
#endif
    }

    //==================================================================================================================
    void Device::destroy(Heap heap)
    {
        core::destroyHeap(DeviceImpl::of(*this).core, heap);
    }
} // namespace rojoRHI
