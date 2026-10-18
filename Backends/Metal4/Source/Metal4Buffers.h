//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Buffers.h
/// @brief Declares the retire hooks of Metal 4 buffers and heaps, which `createDevice` registers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/Records.h"

namespace rojoRHI::metal4 {
    /// Releases the native buffer of a retiring record, after it leaves the address map and, unless it is placed, the device
    /// residency set. `context` is the `DeviceImpl`; a null payload is skipped. It runs under the buffer pool's lock.
    void retireBuffer(void* context, core::BufferRecord& record);

    /// Releases the native heap of a retiring record, after it leaves the device residency set. `context` is the
    /// `DeviceImpl`; a null payload is skipped. It runs under the heap pool's lock.
    void retireHeap(void* context, core::HeapRecord& record);
} // namespace rojoRHI::metal4
