//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Copies.cpp
/// @brief Implements the D3D12 copy entries, which abort: no copy is recorded yet.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include <rojoRHI/RHI.h>

#include <cstdint>

namespace rojoRHI {
    //==================================================================================================================
    void CommandList::copyBuffer(Buffer, uint64_t, Buffer, uint64_t, uint64_t)
    {
        d3d12::notImplemented("CommandList::copyBuffer");
    }

    //==================================================================================================================
    void CommandList::copyBufferToTexture(Buffer, const BufferTextureLayout&, Texture, const TextureRegion&)
    {
        d3d12::notImplemented("CommandList::copyBufferToTexture");
    }

    //==================================================================================================================
    void CommandList::copyTextureToBuffer(Texture, const TextureRegion&, Buffer, const BufferTextureLayout&)
    {
        d3d12::notImplemented("CommandList::copyTextureToBuffer");
    }

    //==================================================================================================================
    void CommandList::copyTexture(Texture, const TextureRegion&, Texture, const TextureRegion&)
    {
        d3d12::notImplemented("CommandList::copyTexture");
    }
} // namespace rojoRHI
