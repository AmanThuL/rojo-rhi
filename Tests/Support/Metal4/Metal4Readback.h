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
    // Copies the buffer into a new Shared buffer through the device's stager, which commits on the internal upload queue
    // and waits, so it reads Private memory too.
    std::vector<std::byte> readBuffer(Device device, Buffer buffer);

    // `layer` is a face for cube types. The texture is single-sample and its format has a packed texel or block size; the
    // result is tight rows of texels or blocks, every slice of a Tex3D mip in turn. Copies through the device's stager, as
    // `readBuffer` does.
    std::vector<std::byte> readTexture(Device device, Texture texture, uint32_t mip, uint32_t layer);
} // namespace rojoRHI::test
