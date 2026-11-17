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
    // A descriptor of a frame slot's block of the transient ring, with its twin in the CPU-only heap.
    struct TransientSlot
    {
        uint32_t index = 0;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
        D3D12_CPU_DESCRIPTOR_HANDLE cpuTwin = {};
    };

    // The resource binding tier 3 guarantee, reserved slots included.
    inline constexpr uint32_t kMinViewHeap = 1'000'000;

    // The largest view heap created, whatever the device reports.
    inline constexpr uint32_t kMaxViewHeap = 1u << 21;

    // The D3D12 `maxSamplers`.
    inline constexpr uint32_t kSamplerHeap = 2'048;

    inline constexpr uint32_t kTransientPerSlot = 1'024;

    // VU-rojo-device-0001, so the most blocks the transient ring holds.
    inline constexpr uint32_t kMaxFrameSlots = 3;

    // Slot 0 holds a null raw SRV and slot 1 a null raw UAV, so a load or a store through the zero `Addr` reaches a
    // null descriptor. The transient ring starts after them, and the view allocator reserves both as its leading block.
    inline constexpr uint32_t kHeldViewSlots = 2;

    // The portable `maxViews` floor: the least view heap less the held slots and the largest ring (ADR 0030).
    inline constexpr uint32_t kMinViews = kMinViewHeap - kHeldViewSlots - kMaxFrameSlots * kTransientPerSlot;
    static_assert(kMinViews == 996'926, "the reference states the maxViews floor as 996,926");

    // The core's view and sampler allocators hand out these heaps' slots, so a `ViewIndex` or `SamplerIndex` is the
    // heap index a shader reads.
    class DescriptorHeaps
    {
      public:
        // The view heap holds the OPTIONS19 maximum, capped at `kMaxViewHeap`, or `kMinViewHeap` when the device does
        // not report it. The held slots and `framesInFlight` blocks of the ring, each with a CPU-only twin, are the
        // view allocator's reserved leading block, so `viewSlots` is `maxViews`, at least `kMinViews`. A reported
        // maximum below `kMinViewHeap`, or an unreported one whose heap cannot be created, is `Unsupported`, naming
        // `maxViews`.
        Result<void> create(DeviceImpl& impl);

        uint32_t viewCapacity() const;

        // The slots after the reserved leading block: the device's `maxViews`, the capacity every allocating call on
        // the view allocator takes.
        uint32_t viewSlots() const;

        uint32_t samplerCapacity() const;

        ID3D12DescriptorHeap* views() const;

        ID3D12DescriptorHeap* samplers() const;

        D3D12_CPU_DESCRIPTOR_HANDLE viewCpu(uint32_t index) const;

        D3D12_GPU_DESCRIPTOR_HANDLE viewGpu(uint32_t index) const;

        D3D12_CPU_DESCRIPTOR_HANDLE samplerCpu(uint32_t index) const;

        // One atomic increment and no lock. Claiming more than `kTransientPerSlot` in one frame slot is a library
        // limit: it logs the count and aborts, in every build.
        TransientSlot claimTransient(uint32_t frameSlot);

        // Only once the slot's frame has retired.
        void resetTransient(uint32_t frameSlot);

      private:
        Com<ID3D12DescriptorHeap> m_views;
        Com<ID3D12DescriptorHeap> m_samplers;
        Com<ID3D12DescriptorHeap> m_twins;
        D3D12_CPU_DESCRIPTOR_HANDLE m_viewCpu = {};
        D3D12_GPU_DESCRIPTOR_HANDLE m_viewGpu = {};
        D3D12_CPU_DESCRIPTOR_HANDLE m_samplerCpu = {};
        D3D12_CPU_DESCRIPTOR_HANDLE m_twinCpu = {};
        uint32_t m_viewIncrement = 0;
        uint32_t m_samplerIncrement = 0;
        uint32_t m_viewSlots = 0;
        uint32_t m_viewCapacity = 0;
        uint32_t m_ringBase = 0;
        uint32_t m_ringBlocks = 0;
        std::array<std::atomic<uint32_t>, kMaxFrameSlots> m_ringClaims = {};
    };

    // `width` is a multiple of 4. The UAV, when `writable`, follows the SRV; returns the SRV's index. Each view counts
    // one descriptor write. A view heap with no free run of the slots needed is `OutOfMemory`, naming `maxViews`.
    Result<uint32_t> createRawViews(DeviceImpl& impl, ID3D12Resource* resource, uint64_t width, bool writable);

    // One raw view reaches at most 2^27 elements, so it covers the first 512 MiB of a larger buffer.
    uint64_t rawViewElements(uint64_t width);

    // Reusable once the releasing frame retires.
    void releaseRawViews(DeviceImpl& impl, uint32_t base, uint32_t count);
} // namespace rojoRHI::d3d12
