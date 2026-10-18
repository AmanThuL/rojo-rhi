//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Textures.h
/// @brief Declares the retire hook of Metal 4 textures, which `createDevice` registers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/Records.h"

namespace rojoRHI::metal4 {
    /// Releases the native texture of a retiring record, after it leaves the device residency set unless it is placed.
    /// `context` is the `DeviceImpl`; a null payload is skipped. It runs under the texture pool's lock.
    void retireTexture(void* context, core::TextureRecord& record);
} // namespace rojoRHI::metal4
