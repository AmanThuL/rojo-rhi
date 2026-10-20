//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ListState.cpp
/// @brief Stubs the D3D12 command list pools: the backend acquires no command list yet.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12ListState.h"

#include "D3D12Common.h"
#include "D3D12Device.h"

#include <cstdint>

namespace rojoRHI::d3d12 {
    //==================================================================================================================
    CommandListImpl& ListPools::acquire(uint32_t)
    {
        notImplemented("ListPools::acquire");
    }

    //==================================================================================================================
    void ListPools::reset(uint32_t)
    {
        notImplemented("ListPools::reset");
    }
} // namespace rojoRHI::d3d12
