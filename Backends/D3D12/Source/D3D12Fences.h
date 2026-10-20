//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Fences.h
/// @brief Declares the D3D12 queues' creation and idling, frame pacing, and the release of destroyed fences.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"
#include "D3D12Common.h"

#include <cstddef>
#include <cstdint>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
    /// A fence only its queue signals, each time with the next value, so a destroyed fence is released once every queue has
    /// passed the point it was destroyed at.
    struct QueueProgress
    {
        Com<ID3D12Fence> fence; ///< The progress fence.
        uint64_t value = 0;     ///< The last value submitted for signal on `fence`.
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

        /// Keeps `fence`, whose caller fence was destroyed, until every queue's progress passes its current value.
        void keep(DeviceImpl& impl, ID3D12Fence* fence);

        /// Releases the kept fences every queue's progress has passed.
        void releasePassed(DeviceImpl& impl);

        /// Releases every kept fence, at teardown, once no queue holds work.
        void releaseAll();

        /// Returns the number of fences kept.
        size_t keptCount() const;

      private:
        Com<ID3D12Fence> m_frame;
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
