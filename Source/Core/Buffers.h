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
    // The one site of the description rules that creation and the placement query share.
    void checkBufferDesc(const DeviceCore& core, const BufferDesc& desc);

    // A placed buffer's heap and placement are checked against `requirements`, which the backend computes only then.
    BufferRecord prepareBuffer(
        DeviceCore& core, const BufferDesc& desc, std::span<const std::byte> initial, const Placement& placement, const PlacementRequirements& requirements
    );

    // A full native table is `OutOfMemory`, with nothing counted or tracked, and the caller keeps `record.native`.
    Result<Buffer> addBuffer(DeviceCore& core, BufferRecord record);

    // The tracker does not follow a page: the CPU writes frame data and the GPU only reads it within its frame. On
    // `OutOfMemory` the caller keeps `record.native`.
    Result<Buffer> addFrameArenaPage(DeviceCore& core, BufferRecord record);

    void destroyBuffer(DeviceCore& core, Buffer buffer);

    // The caller checks the device object itself (VU-rojo-core-0004).
    BufferRecord& checkedBuffer(DeviceCore& core, Buffer buffer);

    void checkBufferAddress(const BufferRecord& record, uint64_t offset, uint64_t maxAddrOffset);

    void checkCpuAccess(const BufferRecord& record);

    void checkCopySource(const BufferRecord& record);

    void checkCopyDestination(const BufferRecord& record);

    void checkAddrOffset(uint64_t offset, uint64_t maxAddrOffset);
} // namespace rojoRHI::core
