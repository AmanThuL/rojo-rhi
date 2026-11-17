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
    // `buffer` must be idle on every queue. Copies on a DIRECT queue, allocator, list and fence of its own, made and
    // released by the call, and waits. A failed native call fails the running case.
    std::vector<std::byte> readNativeBuffer(DeviceImpl& impl, ID3D12Resource* buffer, uint64_t size);

    // `texture` must be idle and its subresource in `layout`. Returns tight rows of texels, or of blocks for a
    // block-compressed format, every depth slice of a 3D subresource in turn. Copies as `readNativeBuffer` does, moving
    // the subresource to `D3D12_BARRIER_LAYOUT_COPY_SOURCE` and back.
    std::vector<std::byte> readNativeTexture(DeviceImpl& impl, ID3D12Resource* texture, uint32_t subresource, D3D12_BARRIER_LAYOUT layout);
} // namespace rojoRHI::test
