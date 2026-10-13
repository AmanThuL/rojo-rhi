//----------------------------------------------------------------------------------------------------------------------
/// @file Queue.h
/// @brief Declares the queue object handle and its submission.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/CommandList.h>
#include <rojoRHI/FenceValue.h>
#include <rojoRHI/Swapchain.h>
#include <rojoRHI/TileMapping.h>

#include <span>

namespace rojoRHI {
    /// One of a device's queues. It wraps one backend pointer: copies refer to the same queue, and a default-constructed one
    /// refers to none (VU-rojo-core-0004). The device owns it and it is never destroyed separately; it is used only with the
    /// device that produced it (VU-rojo-core-0005). Operations on one queue are serialized by the caller.
    class Queue
    {
      public:
        /// Submits `lists` for execution in span order. No list starts before every fence in `wait` has reached its value;
        /// every fence in `signal` is set to its value once every list has completed. A submission with no lists only waits
        /// and signals. Every list has ended, is unsubmitted and was acquired in the open frame (VU-rojo-queue-0001), for this
        /// queue's `QueueType` (VU-rojo-queue-0002). Each fence appears at most once in `signal`, with a value above every
        /// value submitted for signal on it and above its initial value (VU-rojo-queue-0003); each `wait` value is at most the
        /// fence's initial value or the greatest value already submitted for signal (VU-rojo-queue-0007). Thread class queue.
        void submit(std::span<const CommandList> lists, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {});

        /// Hands the swapchain's acquired image to the display once all work submitted earlier to this queue completes. It is
        /// the graphics queue (VU-rojo-present-0015); the swapchain holds an image acquired in this frame and not yet presented
        /// (VU-rojo-present-0016), and the call comes before the `Device::endFrame` that closes that frame
        /// (VU-rojo-present-0017). The image was last moved to `Layout::Present` (VU-rojo-present-0018) and no list submitted
        /// afterwards references it (VU-rojo-present-0019). Does not wait. Thread class queue.
        void present(Swapchain swapchain);

        /// Applies `mappings` in order as a queue operation: after all work submitted earlier to this queue and before any work
        /// submitted to it later. Every element obeys the `TileMapping` rules (VU-rojo-sparse-0002 to 0010) and no two elements name the
        /// same tile (VU-rojo-sparse-0012). Work on another queue that accesses a tile whose mapping changes is complete or
        /// ordered against the mapping by fences before it executes (VU-rojo-sparse-0013, checked by the validation tracker). A
        /// heap stays alive while any tile remains mapped from it (VU-rojo-device-0009). Does not wait. Thread class queue.
        void mapTiles(std::span<const TileMapping> mappings);

      private:
        friend struct QueueImpl;
        struct QueueImpl* m_impl = nullptr;
    };
} // namespace rojoRHI
