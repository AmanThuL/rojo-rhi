//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Scopes.cpp
/// @brief Implements the D3D12 scope entries, which abort: no scope is recorded yet.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include <rojoRHI/RHI.h>

#include <string_view>

namespace rojoRHI {
    //==================================================================================================================
    void CommandList::beginScope(std::string_view)
    {
        d3d12::notImplemented("CommandList::beginScope");
    }

    //==================================================================================================================
    void CommandList::endScope()
    {
        d3d12::notImplemented("CommandList::endScope");
    }
} // namespace rojoRHI
