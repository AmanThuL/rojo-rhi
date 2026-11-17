//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Memory.h
/// @brief Declares how the D3D12 backend maps a memory kind to a heap type.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

namespace rojoRHI::d3d12 {
    // `Upload` is GPU_UPLOAD when the device reports `gpuUploadHeap`, UPLOAD otherwise.
    D3D12_HEAP_TYPE heapType(Memory memory, bool gpuUploadHeap);
} // namespace rojoRHI::d3d12
