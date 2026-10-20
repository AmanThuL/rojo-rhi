//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Descriptors.h
/// @brief Declares the D3D12 device's shader-visible view and sampler heaps, the transient ring and raw buffer views.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <cstdint>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
    /// One descriptor of the transient ring a frame slot claims: its index in the view heap, its GPU and CPU handles there,
    /// and the CPU handle of its twin in the CPU-only heap.
    struct TransientSlot
    {
        uint32_t index = 0;                       ///< The descriptor's index in the view heap.
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};     ///< Its GPU handle in the view heap.
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};     ///< Its CPU handle in the view heap.
        D3D12_CPU_DESCRIPTOR_HANDLE cpuTwin = {}; ///< The CPU handle of its twin in the CPU-only heap.
    };

    /// The shader-visible heaps of a device: the view heap every buffer view, texture view and transient descriptor lives in,
    /// and the sampler heap.
    class DescriptorHeaps
    {
      public:
        /// Creates both heaps, the reserved slot 0 and the transient ring on `impl`'s device.
        Result<void> create(DeviceImpl& impl);

        /// Returns the number of descriptors of the view heap.
        uint32_t viewCapacity() const;

        /// Returns the number of descriptors of the sampler heap.
        uint32_t samplerCapacity() const;

        /// Returns the view heap.
        ID3D12DescriptorHeap* views() const;

        /// Returns the sampler heap.
        ID3D12DescriptorHeap* samplers() const;

        /// Returns the CPU handle of view-heap descriptor `index`.
        D3D12_CPU_DESCRIPTOR_HANDLE viewCpu(uint32_t index) const;

        /// Returns the GPU handle of view-heap descriptor `index`.
        D3D12_GPU_DESCRIPTOR_HANDLE viewGpu(uint32_t index) const;

        /// Returns the CPU handle of sampler-heap descriptor `index`.
        D3D12_CPU_DESCRIPTOR_HANDLE samplerCpu(uint32_t index) const;

        /// Claims the next transient descriptor of frame slot `frameSlot`'s block of the ring.
        TransientSlot claimTransient(uint32_t frameSlot);

        /// Makes frame slot `frameSlot`'s block of the ring claimable again, once the slot's frame has retired.
        void resetTransient(uint32_t frameSlot);
    };

    /// Creates the raw SRV of `resource`, a buffer `width` bytes wide (a multiple of 4), and its raw UAV after it when
    /// `writable`, in a range the view allocator hands out; returns the SRV's index.
    Result<uint32_t> createRawViews(DeviceImpl& impl, ID3D12Resource* resource, uint64_t width, bool writable);

    /// Returns the four-byte elements a raw view of a buffer `width` bytes wide covers.
    uint64_t rawViewElements(uint64_t width);

    /// Releases the `count` raw views at `base` to the view allocator, reusable once the releasing frame retires.
    void releaseRawViews(DeviceImpl& impl, uint32_t base, uint32_t count);
} // namespace rojoRHI::d3d12
