//----------------------------------------------------------------------------------------------------------------------
/// @file Buffers.h
/// @brief Declares the checks of the buffer chapter, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Records.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace rojoRHI::core {
    /// Checks a buffer description (VU-rojo-buffer-0003 to 0006) against the device's limits and capabilities; the one site of
    /// those rules, shared by creation and by the placement query.
    void checkBufferDesc(const DeviceCore& core, const BufferDesc& desc);

    /// Checks a buffer description and its initial data (VU-rojo-buffer-0003 to 0008, VU-rojo-core-0006, VU-rojo-core-0007)
    /// against the device's limits and capabilities, and, when `placement.heap` is not null, the placement
    /// (VU-rojo-core-0001 to 0003 for the heap, VU-rojo-heap-0003 to 0006) against `requirements`, which the backend
    /// computes only for a placed buffer. It returns the record of the buffer with no native payload. A backend calls it first
    /// in `createBuffer`.
    BufferRecord prepareBuffer(
        DeviceCore& core, const BufferDesc& desc, std::span<const std::byte> initial, const Placement& placement, const PlacementRequirements& requirements
    );

    /// Stores `record`, which `prepareBuffer` returned, in the buffer pool, counts it against its heap, starts the validation
    /// tracker following it and returns its handle.
    Buffer addBuffer(DeviceCore& core, BufferRecord record);

    /// Stores `record`, a frame-arena page that `prepareBuffer` returned with no placement, in the buffer pool and returns its
    /// handle. The validation tracker does not follow a page: the CPU writes frame data and the GPU only reads it within its
    /// frame.
    Buffer addFrameArenaPage(DeviceCore& core, BufferRecord record);

    /// Destroys `buffer`, deferred as `DeviceCore::destroy` does, and releases its claim on its heap.
    void destroyBuffer(DeviceCore& core, Buffer buffer);

    /// Finds the live record `buffer` names (VU-rojo-core-0001 to 0003, VU-rojo-core-0004 is the caller's).
    BufferRecord& checkedBuffer(DeviceCore& core, Buffer buffer);

    /// Checks the arguments of `Device::address` for `record` against the device's `maxAddrOffset` (VU-rojo-buffer-0011, 0012,
    /// 0013).
    void checkBufferAddress(const BufferRecord& record, uint64_t offset, uint64_t maxAddrOffset);

    /// Checks that `record` is persistently mapped (VU-rojo-buffer-0014).
    void checkCpuAccess(const BufferRecord& record);

    /// Checks that `record` may be the source of a copy command (VU-rojo-buffer-0002); the one site of the rule, shared by every
    /// copy.
    void checkCopySource(const BufferRecord& record);

    /// Checks that `record` may be the destination of a copy command (VU-rojo-buffer-0001); the one site of the rule, shared by
    /// every copy.
    void checkCopyDestination(const BufferRecord& record);

    /// Checks that `offset` bytes into a buffer is less than `maxAddrOffset` (VU-rojo-buffer-0011); the one site of the rule.
    void checkAddrOffset(uint64_t offset, uint64_t maxAddrOffset);
} // namespace rojoRHI::core
