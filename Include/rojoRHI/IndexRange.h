//----------------------------------------------------------------------------------------------------------------------
/// @file IndexRange.h
/// @brief Declares a caller-owned contiguous range of heap slots.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// A caller-owned contiguous range of heap slots. Slot `base + i` for `i` below `count` is the view index or sampler index
    /// a shader fetches. A range passed to RojoRHI is a value `Device::reserveViews` or `Device::reserveSamplers` returned,
    /// unmodified, that has not been released (VU-rojo-texture-0031).
    struct IndexRange
    {
        /// The heap a range lives in.
        enum class Kind : uint8_t
        {
            Views,    ///< The view heap.
            Samplers, ///< The sampler heap.
        };

        /// First slot of the range.
        uint32_t base = 0;
        /// Slots in the range.
        uint32_t count = 0;
        /// Heap the range lives in, set by the reserving call.
        Kind kind = Kind::Views;
    };
} // namespace rojoRHI
