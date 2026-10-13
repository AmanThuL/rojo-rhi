//----------------------------------------------------------------------------------------------------------------------
/// @file HeapDesc.h
/// @brief Declares the description of a placement heap.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Memory.h>

#include <cstdint>
#include <string_view>

namespace rojoRHI {
    /// Describes a placement heap for `Device::createHeap`: one device allocation that buffers and textures are placed inside
    /// at explicit offsets. It tracks no hazards: placed resources whose byte ranges overlap alias, and the caller orders
    /// them. A backend may round `size` up; the usable size is never less than `size`. `size` is greater than zero
    /// (VU-rojo-heap-0001).
    struct HeapDesc
    {
        /// Bytes.
        uint64_t size = 0;
        /// Memory class of every resource placed inside.
        Memory memory = Memory::Gpu;
        /// Debug label; empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };
} // namespace rojoRHI
