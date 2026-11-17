//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4TextureDescriptor.h
/// @brief Declares the Metal 4 texture type and texture descriptor of a texture description.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <Metal/Metal.hpp>

#include <cstdint>

namespace rojoRHI::metal4 {
    MTL::TextureType textureType(TextureType type, uint32_t samples);

    // Returns +1; the caller releases it.
    MTL::TextureDescriptor* textureDescriptor(const TextureDesc& desc, bool placed);
} // namespace rojoRHI::metal4
