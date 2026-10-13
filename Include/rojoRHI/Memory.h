//----------------------------------------------------------------------------------------------------------------------
/// @file Memory.h
/// @brief Declares the memory classes of buffers and heaps.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The memory class of a buffer or heap, named by intent rather than by backend heap type. It fixes a buffer's copy
    /// roles: `Gpu` is a copy source and destination, `Upload` a copy source, `Readback` a copy destination.
    enum class Memory : uint8_t
    {
        Gpu,      ///< Device-local; not CPU-visible.
        Upload,   ///< CPU writes, GPU reads; persistently mapped.
        Readback, ///< GPU writes, CPU reads; persistently mapped.
    };
} // namespace rojoRHI
