//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Barriers.cpp
/// @brief Implements the D3D12 barrier entry, which aborts: no barrier batch is lowered yet.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include <rojoRHI/RHI.h>

#include <span>

namespace rojoRHI {
    //==================================================================================================================
    void CommandList::barrier(std::span<const GlobalBarrier>, std::span<const BufferBarrier>, std::span<const TextureBarrier>)
    {
        d3d12::notImplemented("CommandList::barrier");
    }
} // namespace rojoRHI
