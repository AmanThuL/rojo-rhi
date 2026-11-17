//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Fences.h
/// @brief Declares the D3D12 queues' creation and idling, frame pacing, and the release of destroyed fences.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"
#include "D3D12Common.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
    // Only its queue signals it, each time with the next value, so a destroyed fence is released once every queue has
    // passed the value its progress had when the fence retired. The compute and copy queues signal it after each
    // submission that waits or signals and joins no frame: one with no lists, or with more than one signal.
    struct QueueProgress
    {
        Com<ID3D12Fence> fence;
        // Only the queue's submissions write it, which the caller serializes; a fence's retirement reads it from
        // another thread.
        std::atomic<uint64_t> value = 0;
    };

    // Also keeps the fences of destroyed caller fences, which a queue may still wait on or signal.
    class FrameState
    {
      public:
        Result<void> create(DeviceImpl& impl);

        ID3D12Fence* fence() const;

        // Takes over the one reference `fence` carries, until every queue's progress passes its current value.
        void keep(DeviceImpl& impl, ID3D12Fence* fence);

        // Reads a queue's progress fence, one counted native host call, only when a kept fence waits for that queue.
        void releasePassed(DeviceImpl& impl);

        // At teardown, once no queue holds work.
        void releaseAll();

        size_t keptCount() const;

      private:
        struct Kept
        {
            Com<ID3D12Fence> fence;
            std::array<uint64_t, 3> progress = {};
        };

        Com<ID3D12Fence> m_frame;
        mutable std::mutex m_mutex; // Guards `m_kept` and `m_seen`; a leaf lock.
        std::vector<Kept> m_kept;
        std::array<uint64_t, 3> m_seen = {};
    };

    Result<void> createQueues(DeviceImpl& impl);

    void idleQueues(DeviceImpl& impl);

    // `context` is the DeviceImpl; a null payload is skipped. Runs under the fence pool's lock.
    void retireFence(void* context, core::FenceRecord& record);

    // `context` is the DeviceImpl.
    void waitFrame(void* context, uint64_t frame);

    // Also releases the kept fences every queue has passed and frees the destroyed swapchains whose frame the core has
    // waited for. `context` is the DeviceImpl.
    void reuseSlot(void* context, uint32_t slot);
} // namespace rojoRHI::d3d12
