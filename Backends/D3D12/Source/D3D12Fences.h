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
    /// A fence only its queue signals, each time with the next value, so a destroyed fence is released once every queue has
    /// passed the value its progress had when the fence retired, which covers every submission made before the destruction.
    /// The compute and copy queues signal it after each submission that waits or signals and joins no frame: one with no
    /// lists, or with more than one signal.
    struct QueueProgress
    {
        Com<ID3D12Fence> fence; ///< The progress fence.
        /// The last value submitted for signal on `fence`. Only the queue's submissions write it, which the caller serializes;
        /// a fence's retirement reads it from another thread.
        std::atomic<uint64_t> value = 0;
    };

    /// The frame fence the graphics queue signals with each frame's number, and the fences of destroyed caller fences that a
    /// queue may still wait on or signal.
    class FrameState
    {
      public:
        /// Creates the frame fence on `impl`'s device.
        Result<void> create(DeviceImpl& impl);

        /// Returns the frame fence.
        ID3D12Fence* fence() const;

        /// Keeps `fence`, whose caller fence was destroyed, until every queue's progress passes its current value. Takes over
        /// the one reference `fence` carries.
        void keep(DeviceImpl& impl, ID3D12Fence* fence);

        /// Releases the kept fences every queue's progress has passed, reading a queue's progress fence, one counted native
        /// host call, only when a kept fence waits for that queue.
        void releasePassed(DeviceImpl& impl);

        /// Releases every kept fence, at teardown, once no queue holds work.
        void releaseAll();

        /// Returns the number of fences kept.
        size_t keptCount() const;

      private:
        /// A destroyed fence's native fence and, per `QueueType`, the progress value that queue must reach before it goes.
        struct Kept
        {
            Com<ID3D12Fence> fence;                ///< The kept fence.
            std::array<uint64_t, 3> progress = {}; ///< Per `QueueType`, the progress value the queue must reach.
        };

        Com<ID3D12Fence> m_frame;
        mutable std::mutex m_mutex;          ///< Guards `m_kept` and `m_seen`; a leaf lock.
        std::vector<Kept> m_kept;            ///< The fences of destroyed caller fences a queue may still use.
        std::array<uint64_t, 3> m_seen = {}; ///< Per `QueueType`, the greatest progress value read so far.
    };

    /// Creates the native queue of each `QueueType` of `impl`, and its progress fence.
    Result<void> createQueues(DeviceImpl& impl);

    /// Blocks until every queue of `impl` that exists has finished the work submitted to it.
    void idleQueues(DeviceImpl& impl);

    /// Keeps the fence of a retiring fence record until no queue uses it. `context` is the `DeviceImpl`; a null payload is
    /// skipped. It runs under the fence pool's lock.
    void retireFence(void* context, core::FenceRecord& record);

    /// Blocks until frame `frame` has completed on the graphics queue. `context` is the `DeviceImpl`.
    void waitFrame(void* context, uint64_t frame);

    /// Returns frame slot `slot`'s command lists and transient descriptors for reuse and releases the kept fences every queue
    /// has passed. `context` is the `DeviceImpl`.
    void reuseSlot(void* context, uint32_t slot);
} // namespace rojoRHI::d3d12
