//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Clears.cpp
/// @brief Implements the D3D12 texture clear entry, which aborts: no clear is recorded yet.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include <rojoRHI/RHI.h>

namespace rojoRHI {
    //==================================================================================================================
    void CommandList::clearTexture(Texture, const SubresourceRange&, const ClearValue&)
    {
        d3d12::notImplemented("CommandList::clearTexture");
    }
} // namespace rojoRHI
