//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Stager.h
/// @brief Declares the D3D12 stager, which copies a resource's initial data on the device's internal upload queue.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
    // In D3D12 subresource order.
    struct SubresourceData
    {
        std::span<const std::byte> bytes; // Empty leaves the subresource's contents undefined.
        uint64_t bytesPerRow = 0;
        uint64_t bytesPerImage = 0; // For a 3D texture.
    };

    // The upload queue, `rojorhi.upload`, is a DIRECT queue no `QueueType` names: it never runs a caller's lists and
    // never signals a caller's fence. Each upload fills a committed UPLOAD staging buffer, records the copy into the
    // stager's one allocator and list, executes it, signals the stager's fence and waits for that value before it
    // returns, so the data is complete before any work any queue submits afterwards. Uploads run one at a time under a
    // leaf mutex.
    class Stager
    {
      public:
        Result<void> create(DeviceImpl& impl);

        // `bytes` is not empty. Counts the staging buffer as one `nativeCreateCalls`, its Map and each
        // GetCompletedValue and SetEventOnCompletion of the wait as one `nativeHostCalls`, and the execution and the
        // signal as one `nativeSubmitCalls` each.
        Result<void> uploadBuffer(DeviceImpl& impl, ID3D12Resource* destination, std::span<const std::byte> bytes);

        // `destination` was created in LAYOUT_COPY_DEST. One staging buffer holds every non-empty entry, its rows
        // repacked to the footprint's 256-byte pitch, and one texture barrier then moves every subresource, empty ones
        // included, to the generic LAYOUT_SHADER_RESOURCE, which direct and compute queues accept. With every entry
        // empty it records the barrier alone. Counts and fails as `uploadBuffer` does.
        Result<void> uploadTexture(DeviceImpl& impl, ID3D12Resource* destination, std::span<const SubresourceData> subresources);

        // Waits for the last upload; returns at once when nothing was uploaded. Device teardown calls it, so it counts
        // nothing.
        void idle();

      private:
        // The caller holds `m_mutex`, and the last upload has completed.
        Result<void> reset(DeviceImpl& impl);

        // Closes the list, executes it on the upload queue, signals the next fence value and waits for it, then reports
        // a removed device. A signal that fails on a device that is not removed is logged and aborts, in every build,
        // since the list may still run. The caller holds `m_mutex` and has recorded into the list since its `Reset`.
        Result<void> submit(DeviceImpl& impl);

        std::mutex m_mutex;
        Com<ID3D12CommandQueue> m_queue;
        Com<ID3D12CommandAllocator> m_allocator;
        Com<ID3D12GraphicsCommandList9> m_list;
        Com<ID3D12Fence> m_fence;
        uint64_t m_value = 0;
    };
} // namespace rojoRHI::d3d12
