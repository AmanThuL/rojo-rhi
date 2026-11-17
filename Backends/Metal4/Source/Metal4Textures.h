//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Textures.h
/// @brief Declares the retire hook of Metal 4 textures, which `createDevice` registers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/Records.h"

namespace rojoRHI::metal4 {
    // Runs under the texture pool's lock. A texture that is not placed leaves the device residency set before its
    // release.
    void retireTexture(void* context, core::TextureRecord& record);
} // namespace rojoRHI::metal4
