//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Readback.h
/// @brief Declares the test helpers that read any native D3D12 buffer or texture subresource back to the CPU.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "D3D12Common.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::test {
    /// Returns the first `size` bytes of `buffer`, a buffer of `impl`'s device that no queue is using. It copies them into a
    /// new `READBACK` buffer on a DIRECT queue, allocator, list and fence of its own, made and released by the call, and waits.
    /// A failed native call fails the running case.
    std::vector<std::byte> readNativeBuffer(DeviceImpl& impl, ID3D12Resource* buffer, uint64_t size);

    /// Returns the texels of subresource `subresource` of `texture`, a texture of `impl`'s device that no queue is using and
    /// whose subresource is in `layout`, in tight rows of texels, or of blocks for a block-compressed format: every depth
    /// slice of a 3D subresource, one after the other. As `readNativeBuffer` does, it copies on a queue of its own and waits;
    /// barriers move the subresource from `layout` to `D3D12_BARRIER_LAYOUT_COPY_SOURCE` for the copy and back to `layout`.
    std::vector<std::byte> readNativeTexture(DeviceImpl& impl, ID3D12Resource* texture, uint32_t subresource, D3D12_BARRIER_LAYOUT layout);
} // namespace rojoRHI::test
