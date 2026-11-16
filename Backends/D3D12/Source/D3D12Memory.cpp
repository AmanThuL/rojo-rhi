//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Memory.cpp
/// @brief Implements the mapping from a memory kind to a D3D12 heap type.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Memory.h"

namespace rojoRHI::d3d12 {
    //==================================================================================================================
    D3D12_HEAP_TYPE heapType(Memory memory, bool gpuUploadHeap)
    {
        switch (memory)
        {
        case Memory::Gpu: return D3D12_HEAP_TYPE_DEFAULT;
        case Memory::Upload: return gpuUploadHeap ? D3D12_HEAP_TYPE_GPU_UPLOAD : D3D12_HEAP_TYPE_UPLOAD;
        case Memory::Readback: return D3D12_HEAP_TYPE_READBACK;
        }
        return D3D12_HEAP_TYPE_DEFAULT;
    }
} // namespace rojoRHI::d3d12
