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
    /// The initial data of one subresource, in D3D12 subresource order.
    struct SubresourceData
    {
        std::span<const std::byte> bytes; ///< The subresource's texels; empty leaves its contents undefined.
        uint64_t bytesPerRow = 0;         ///< The distance between rows of `bytes`.
        uint64_t bytesPerImage = 0;       ///< The distance between depth slices of `bytes`, for a 3D texture.
    };

    /// Copies initial data into device memory on the internal upload queue, one upload at a time. The queue, `rojorhi.upload`,
    /// is a DIRECT queue no `QueueType` names: it never runs a caller's lists and never signals a caller's fence. Each upload
    /// fills a committed `UPLOAD` staging buffer, records the copy into the stager's one allocator and list, executes it,
    /// signals the stager's fence and waits for that value before it returns, so the data is complete before any work any queue
    /// submits afterwards. Uploads run one at a time under a leaf mutex.
    class Stager
    {
      public:
        /// Creates the upload queue, its allocator, its list (closed) and its fence on `impl`'s device.
        Result<void> create(DeviceImpl& impl);

        /// Copies `bytes`, which are not empty, into the start of the buffer `destination` and waits for the copy. It counts
        /// the staging buffer as one `nativeCreateCalls`, its `Map` and each `GetCompletedValue` and `SetEventOnCompletion` of
        /// the wait as one `nativeHostCalls`, and the execution and the signal as one `nativeSubmitCalls` each. A failed native
        /// call is the error `d3d12::failure` gives.
        Result<void> uploadBuffer(DeviceImpl& impl, ID3D12Resource* destination, std::span<const std::byte> bytes);

        /// Copies each non-empty entry of `subresources`, one per subresource of the texture `destination` in D3D12 subresource
        /// order, into it and waits, counting and failing as `uploadBuffer` does. `destination` was created in
        /// `D3D12_BARRIER_LAYOUT_COPY_DEST`; one staging buffer holds every entry with its rows repacked to the footprint's
        /// 256-byte pitch, and one texture barrier then moves every subresource, empty ones included, to the generic
        /// `D3D12_BARRIER_LAYOUT_SHADER_RESOURCE`, which direct and compute queues accept. With every entry empty it makes no
        /// staging buffer and records the barrier alone, which then waits for nothing and grants no access.
        Result<void> uploadTexture(DeviceImpl& impl, ID3D12Resource* destination, std::span<const SubresourceData> subresources);

        /// Blocks until the last upload has completed; returns at once when nothing was uploaded. Device teardown calls it, so
        /// it counts nothing.
        void idle();

      private:
        /// Resets the allocator and the list for a new recording. The caller holds `m_mutex`, and the last upload has completed.
        Result<void> reset(DeviceImpl& impl);

        /// Closes the list, executes it on the upload queue, signals the next fence value and waits for it, then reports a
        /// removed device. A signal that fails on a device that is not removed is logged and aborts, in every build, since the
        /// list may still run. The caller holds `m_mutex` and has recorded into the list since its `Reset`.
        Result<void> submit(DeviceImpl& impl);

        std::mutex m_mutex;
        Com<ID3D12CommandQueue> m_queue;
        Com<ID3D12CommandAllocator> m_allocator;
        Com<ID3D12GraphicsCommandList9> m_list;
        Com<ID3D12Fence> m_fence;
        uint64_t m_value = 0;
    };
} // namespace rojoRHI::d3d12
