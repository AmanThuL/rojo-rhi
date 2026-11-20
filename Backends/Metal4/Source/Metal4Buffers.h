//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Buffers.h
/// @brief Declares the Metal 4 buffer payload and the retire hooks of buffers and heaps, which `createDevice` registers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Metal4Residency.h"

#include <Metal/Metal.hpp>

#include <cstdint>
#include <memory>

namespace rojoRHI::metal4 {
    // Read once at creation, so recording reads the address and length with no message to the buffer. Fixed until the
    // retire hook frees it.
    struct BufferPayload
    {
        MTL::Buffer* buffer = nullptr; // One retained reference.
        uint64_t address = 0;
        uint64_t length = 0; // The bytes Metal allocated, at least the requested size.
        // A frame-arena page's deferred join of the device residency set (`Residency::addDeferred`); unused by other buffers.
        // `retireBuffer` removes the buffer from the set before it frees the payload, which takes the node off the list.
        Residency::Pending residencyJoin;
    };

    // One acquire load and no lock (`HandlePool::native`); null for the null handle.
    inline const BufferPayload* bufferPayload(core::DeviceCore& core, Buffer buffer)
    {
        return static_cast<const BufferPayload*>(core.pool<Buffer>().native(buffer));
    }

    // The caller stores it in the record's `native` and releases ownership once the pool holds the record.
    std::unique_ptr<BufferPayload> makeBufferPayload(core::DeviceCore& core, MTL::Buffer* buffer);

    // Runs under the buffer pool's lock. The buffer leaves the address map and, unless placed, the device residency set
    // first.
    void retireBuffer(void* context, core::BufferRecord& record);

    // Runs under the heap pool's lock.
    void retireHeap(void* context, core::HeapRecord& record);
} // namespace rojoRHI::metal4
