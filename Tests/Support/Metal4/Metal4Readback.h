//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Readback.h
/// @brief Declares the test helpers that read any Metal 4 buffer or texture back to the CPU, Private memory included.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rojoRHI::test {
    /// Returns every byte of `buffer`, a live buffer of `device`. It copies the buffer into a new Shared buffer through the
    /// device's stager, which commits on the internal upload queue and waits, so it reads Private memory too.
    std::vector<std::byte> readBuffer(Device device, Buffer buffer);

    /// Returns the texels of mip `mip` of layer `layer` (a face, for cube types) of `texture`, a live single-sample texture of
    /// `device` whose format has a packed texel or block size, in tight rows of texels or blocks: every slice of a Tex3D mip,
    /// one after the other. It copies the subresource into a new Shared buffer through the device's stager, as `readBuffer`
    /// does.
    std::vector<std::byte> readTexture(Device device, Texture texture, uint32_t mip, uint32_t layer);
} // namespace rojoRHI::test
