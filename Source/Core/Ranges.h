//----------------------------------------------------------------------------------------------------------------------
/// @file Ranges.h
/// @brief Declares the reservation and release of caller-owned heap slot ranges of chapter 04.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <cstdint>

namespace rojoRHI::core {
    Result<IndexRange> reserveViews(DeviceCore& core, uint32_t count);

    Result<IndexRange> reserveSamplers(DeviceCore& core, uint32_t count);

    // The slots free when the open frame, or the next one, retires; the textures they described stop counting those views.
    void releaseRange(DeviceCore& core, IndexRange range);
} // namespace rojoRHI::core
