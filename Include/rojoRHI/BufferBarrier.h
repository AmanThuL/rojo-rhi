//----------------------------------------------------------------------------------------------------------------------
/// @file BufferBarrier.h
/// @brief Declares the barrier scoped to a range of one buffer and the constant that names the rest of a buffer.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Access.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/Stage.h>

#include <cstdint>

namespace rojoRHI {
    /// A `BufferBarrier::size` that extends the range to the end of the buffer.
    inline constexpr uint64_t kWholeSize = ~uint64_t{ 0 };

    /// A memory dependency scoped to one range of one buffer. `offset` is below the buffer's size and `size` is `kWholeSize` or
    /// above zero with `offset + size` within the buffer (VU-rojo-sync-0010); the accesses are buffer accesses
    /// (VU-rojo-sync-0011); the sides obey VU-rojo-sync-0001 to 0003.
    struct BufferBarrier
    {
        /// The buffer whose memory the barrier orders.
        Buffer buffer;
        /// First byte of the range.
        uint64_t offset = 0;
        /// Bytes of the range, or `kWholeSize`.
        uint64_t size = kWholeSize;
        /// Stages of earlier work the barrier waits for.
        Stage srcStage = Stage::None;
        /// Accesses of earlier work the barrier makes visible.
        Access srcAccess = Access::None;
        /// Stages of later work that wait.
        Stage dstStage = Stage::None;
        /// Accesses of later work that see the source writes.
        Access dstAccess = Access::None;
    };
} // namespace rojoRHI
