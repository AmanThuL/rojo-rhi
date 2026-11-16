//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Buffers.h
/// @brief Declares the Metal 4 buffer payload and the retire hooks of buffers and heaps, which `createDevice` registers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"

#include <Metal/Metal.hpp>

#include <cstdint>
#include <memory>

namespace rojoRHI::metal4 {
    /// The payload of a buffer record: the native buffer and the GPU address and length Metal reports for it, read once at
    /// creation, so recording reads them with no message to the buffer. It is fixed from creation to retirement, when the
    /// retire hook frees it.
    struct BufferPayload
    {
        MTL::Buffer* buffer = nullptr; ///< The native buffer, one retained reference.
        uint64_t address = 0;          ///< `buffer->gpuAddress()`.
        uint64_t length = 0;           ///< `buffer->length()`: the bytes Metal allocated, at least the requested size.
    };

    /// Returns the payload of `buffer` with one acquire load and no lock (`HandlePool::native`); null for the null handle.
    inline const BufferPayload* bufferPayload(core::DeviceCore& core, Buffer buffer)
    {
        return static_cast<const BufferPayload*>(core.pool<Buffer>().native(buffer));
    }

    /// Makes the payload of `buffer`, reading its GPU address and length, and counts the allocation on `core`. The caller
    /// stores it in the record's `native` and releases ownership once the pool holds the record; the retire hook frees it.
    std::unique_ptr<BufferPayload> makeBufferPayload(core::DeviceCore& core, MTL::Buffer* buffer);

    /// Releases the native buffer of a retiring record and frees its payload, after the buffer leaves the address map and,
    /// unless it is placed, the device residency set. `context` is the `DeviceImpl`; a null payload is skipped. It runs under
    /// the buffer pool's lock.
    void retireBuffer(void* context, core::BufferRecord& record);

    /// Releases the native heap of a retiring record, after it leaves the device residency set. `context` is the
    /// `DeviceImpl`; a null payload is skipped. It runs under the heap pool's lock.
    void retireHeap(void* context, core::HeapRecord& record);
} // namespace rojoRHI::metal4
