//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Descriptors.h
/// @brief Declares the D3D12 device's shader-visible view and sampler heaps, the transient ring and raw buffer views.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <array>
#include <atomic>
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

    /// The least `maxViews` the device accepts, the portable floor and the resource binding tier 3 guarantee.
    inline constexpr uint32_t kMinViewHeap = 1'000'000;

    /// The largest view heap the device creates, whatever the device reports.
    inline constexpr uint32_t kMaxViewHeap = 1u << 21;

    /// The descriptors of the shader-visible sampler heap, the D3D12 `maxSamplers`.
    inline constexpr uint32_t kSamplerHeap = 2'048;

    /// The transient descriptors each frame slot's block of the ring holds.
    inline constexpr uint32_t kTransientPerSlot = 1'024;

    /// The most frame slots a device has (VU-rojo-device-0001), so the most blocks the transient ring holds.
    inline constexpr uint32_t kMaxFrameSlots = 3;

    /// The view-heap slots held for the device's life: slot 0 holds a null raw SRV and slot 1 a null raw UAV, so a load or
    /// a store through the zero `Addr` reaches a null descriptor. The transient ring starts after them, and the view
    /// allocator reserves both as its leading block.
    inline constexpr uint32_t kHeldViewSlots = 2;

    /// The shader-visible heaps of a device: the view heap every buffer view, texture view and transient descriptor lives in,
    /// and the sampler heap. The core's view and sampler allocators hand out their slots, so a `ViewIndex` or `SamplerIndex`
    /// is the heap index a shader reads. Each heap holds its own reference, released with the device's other state.
    class DescriptorHeaps
    {
      public:
        /// Creates both heaps, the held slots 0 and 1 and the transient ring on `impl`'s device. The view heap holds the
        /// `D3D12_FEATURE_D3D12_OPTIONS19` maximum, capped at `kMaxViewHeap`, or, when the device does not report it,
        /// `kMinViewHeap` descriptors more than the reserved block. Slot 0 holds a null raw SRV and slot 1 a null raw UAV for
        /// the device's life, so the zero `Addr` names no buffer, and the ring is `framesInFlight` blocks of
        /// `kTransientPerSlot` slots right after them, each with a twin in a CPU-only heap. The view allocator reserves those
        /// slots as its leading block, so `viewSlots`, the heap less the block, is `maxViews`. A reported maximum that leaves
        /// fewer than `kMinViewHeap`, or an unreported one whose heap cannot be created, is `Unsupported`, naming `maxViews`.
        Result<void> create(DeviceImpl& impl);

        /// Returns the number of descriptors of the view heap.
        uint32_t viewCapacity() const;

        /// Returns the view-heap slots the view allocator hands out, after the reserved leading block: the device's
        /// `maxViews`, and the capacity every allocating call on the view allocator takes.
        uint32_t viewSlots() const;

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

        /// Claims the next transient descriptor of frame slot `frameSlot`'s block of the ring with one atomic increment and no
        /// lock. Claiming more than `kTransientPerSlot` in one frame slot is a library limit: it logs the count and aborts, in
        /// every build.
        TransientSlot claimTransient(uint32_t frameSlot);

        /// Makes frame slot `frameSlot`'s block of the ring claimable again, once the slot's frame has retired.
        void resetTransient(uint32_t frameSlot);

      private:
        Com<ID3D12DescriptorHeap> m_views;                                   ///< The shader-visible view heap.
        Com<ID3D12DescriptorHeap> m_samplers;                                ///< The shader-visible sampler heap.
        Com<ID3D12DescriptorHeap> m_twins;                                   ///< The CPU-only twins of the ring's descriptors.
        D3D12_CPU_DESCRIPTOR_HANDLE m_viewCpu = {};                          ///< The CPU handle of view-heap descriptor 0.
        D3D12_GPU_DESCRIPTOR_HANDLE m_viewGpu = {};                          ///< The GPU handle of view-heap descriptor 0.
        D3D12_CPU_DESCRIPTOR_HANDLE m_samplerCpu = {};                       ///< The CPU handle of sampler-heap descriptor 0.
        D3D12_CPU_DESCRIPTOR_HANDLE m_twinCpu = {};                          ///< The CPU handle of the first twin.
        uint32_t m_viewIncrement = 0;                                        ///< Bytes between two view descriptors.
        uint32_t m_samplerIncrement = 0;                                     ///< Bytes between two sampler descriptors.
        uint32_t m_viewSlots = 0;                                            ///< Slots after the reserved leading block.
        uint32_t m_viewCapacity = 0;                                         ///< Descriptors of the view heap.
        uint32_t m_ringBase = 0;                                             ///< The view-heap index of the ring's first slot.
        uint32_t m_ringBlocks = 0;                                           ///< Blocks of the ring, one per frame slot.
        std::array<std::atomic<uint32_t>, kMaxFrameSlots> m_ringClaims = {}; ///< Slots claimed in each frame slot's block.
    };

    /// Creates the raw SRV of `resource`, a buffer `width` bytes wide (a multiple of 4), and its raw UAV after it when
    /// `writable`, in a range the view allocator hands out; returns the SRV's index. Each view covers `rawViewElements(width)`
    /// elements and counts one descriptor write. A view heap with no free run of the slots needed is `OutOfMemory`, naming
    /// `maxViews`.
    Result<uint32_t> createRawViews(DeviceImpl& impl, ID3D12Resource* resource, uint64_t width, bool writable);

    /// Returns the four-byte elements a raw view of a buffer `width` bytes wide covers: one raw view reaches at most 2^27
    /// elements (`D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP`), the first 512 MiB of a larger buffer.
    uint64_t rawViewElements(uint64_t width);

    /// Releases the `count` raw views at `base`, a range `createRawViews` returned, to the view allocator, reusable once the
    /// releasing frame retires. The core's teardown passes buffers to their retire hooks before it drains the allocator, so
    /// the range is always held; a range that is not is an internal error.
    void releaseRawViews(DeviceImpl& impl, uint32_t base, uint32_t count);
} // namespace rojoRHI::d3d12
