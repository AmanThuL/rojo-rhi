//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Buffers.h
/// @brief Declares the retire hooks of D3D12 buffers and heaps, which createDevice registers, and the frame-arena pages.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/FrameArena.h"
#include "Core/Records.h"

#include <cstdint>

namespace rojoRHI::d3d12 {
    // `context` is the DeviceImpl; a null payload is skipped. Runs under the buffer pool's lock.
    void retireBuffer(void* context, core::BufferRecord& record);

    // `context` is the DeviceImpl; a null payload is skipped. Runs under the heap pool's lock.
    void retireHeap(void* context, core::HeapRecord& record);

    // An Upload buffer with one raw SRV. `context` is the DeviceCore, whose retire hooks' context is the DeviceImpl.
    // The arena calls it under its lock while it claims a page.
    Result<core::ArenaPage> createArenaPage(void* context, uint64_t size);
} // namespace rojoRHI::d3d12
