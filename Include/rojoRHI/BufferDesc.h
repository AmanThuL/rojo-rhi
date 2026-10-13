//----------------------------------------------------------------------------------------------------------------------
/// @file BufferDesc.h
/// @brief Declares the description of a buffer.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/BufferUsage.h>
#include <rojoRHI/Memory.h>

#include <cstdint>
#include <string_view>

namespace rojoRHI {
    /// Describes a buffer for `Device::createBuffer`. `size` is greater than zero and at most `Limits::maxBufferSize`
    /// (VU-rojo-buffer-0006).
    struct BufferDesc
    {
        /// Bytes.
        uint64_t size = 0;
        /// Memory class.
        Memory memory = Memory::Gpu;
        /// Index, IndirectArgs, ShaderWrite and Sparse.
        BufferUsage usage = BufferUsage::None;
        /// Debug label; empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };
} // namespace rojoRHI
