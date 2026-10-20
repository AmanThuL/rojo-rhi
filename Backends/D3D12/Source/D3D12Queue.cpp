//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Queue.cpp
/// @brief Stubs the queue entries of the D3D12 backend: submission, presentation and tile mapping abort until they are implemented.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include <rojoRHI/RHI.h>

#include <span>

namespace rojoRHI {
    //==================================================================================================================
    void Queue::submit(std::span<const CommandList>, std::span<const FenceValue>, std::span<const FenceValue>)
    {
        d3d12::notImplemented("Queue::submit");
    }

    //==================================================================================================================
    void Queue::present(Swapchain)
    {
        d3d12::notImplemented("Queue::present");
    }

    //==================================================================================================================
    void Queue::mapTiles(std::span<const TileMapping>)
    {
        d3d12::notImplemented("Queue::mapTiles");
    }
} // namespace rojoRHI
