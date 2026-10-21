//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Fills.cpp
/// @brief Implements the D3D12 buffer fill entry, which aborts: no fill is recorded yet.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include <rojoRHI/RHI.h>

#include <cstdint>

namespace rojoRHI {
    //==================================================================================================================
    void CommandList::fillBuffer(Buffer, uint64_t, uint64_t, uint32_t)
    {
        d3d12::notImplemented("CommandList::fillBuffer");
    }
} // namespace rojoRHI
