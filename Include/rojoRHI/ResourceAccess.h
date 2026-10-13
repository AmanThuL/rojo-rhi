//----------------------------------------------------------------------------------------------------------------------
/// @file ResourceAccess.h
/// @brief Declares one declared shader access of the validation tracker.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Access.h>
#include <rojoRHI/BufferBarrier.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/Stage.h>
#include <rojoRHI/SubresourceRange.h>

#include <cstdint>

namespace rojoRHI {
    /// One shader access the next draw or dispatch makes through a heap index or an `Addr`, declared through
    /// `CommandList::declareAccess` so the validation tracker can check it. Exactly one of `buffer` and `texture` is not null
    /// (VU-rojo-diag-0001); the range lies inside the resource (VU-rojo-diag-0002); `stage` and `access` are shader ones
    /// (VU-rojo-diag-0003).
    struct ResourceAccess
    {
        /// The buffer accessed; null when `texture` is set.
        Buffer buffer = {};
        /// Byte offset of the accessed range in `buffer`.
        uint64_t offset = 0;
        /// Bytes of the range, or `kWholeSize` for the rest of the buffer.
        uint64_t size = kWholeSize;
        /// The texture accessed; null when `buffer` is set.
        Texture texture = {};
        /// The subresources of `texture` accessed.
        SubresourceRange range;
        /// The shader stage that makes the access: exactly one of the five shader stages.
        Stage stage;
        /// Shader read, shader write, or both.
        Access access;
    };
} // namespace rojoRHI
