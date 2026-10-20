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
    /// Releases the raw views, the resource and the payload of a retiring buffer record. `context` is the `DeviceImpl`; a
    /// null payload is skipped. It runs under the buffer pool's lock.
    void retireBuffer(void* context, core::BufferRecord& record);

    /// Releases the native heap of a retiring heap record. `context` is the `DeviceImpl`; a null payload is skipped. It runs
    /// under the heap pool's lock.
    void retireHeap(void* context, core::HeapRecord& record);

    /// Creates one frame-arena page of `size` bytes: an Upload buffer with one raw SRV. `context` is the `DeviceCore`; its
    /// retire hooks' context is the `DeviceImpl`. The arena calls it while it claims a page, under its lock.
    Result<core::ArenaPage> createArenaPage(void* context, uint64_t size);
} // namespace rojoRHI::d3d12
