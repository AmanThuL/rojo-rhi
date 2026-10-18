//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4TextureDescriptor.h
/// @brief Declares the Metal 4 texture type and texture descriptor of a texture description.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <Metal/Metal.hpp>

#include <cstdint>

namespace rojoRHI::metal4 {
    /// Returns the `MTL::TextureType` of `type` with `samples` samples per texel: `Tex2D` and `Tex2DArray` with more than one
    /// sample are the multisample types.
    MTL::TextureType textureType(TextureType type, uint32_t samples);

    /// Returns a new descriptor (+1; the caller releases it) of a Private texture described by `desc`, which the core accepted:
    /// its type, pixel format, extent, mips, array length in cubes for cube types, samples and usage. The usage adds
    /// `MTL::TextureUsagePixelFormatView` when the format has an sRGB counterpart or a stencil plane, so its views may
    /// reinterpret the format. A `placed` descriptor is untracked, matching the heaps it is placed in.
    MTL::TextureDescriptor* textureDescriptor(const TextureDesc& desc, bool placed);
} // namespace rojoRHI::metal4
