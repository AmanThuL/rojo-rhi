//----------------------------------------------------------------------------------------------------------------------
/// @file Ranges.h
/// @brief Declares the reservation and release of caller-owned heap slot ranges of chapter 04.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <cstdint>

namespace rojoRHI::core {
    /// Reserves `count` contiguous slots of the view heap (VU-rojo-texture-0032). No contiguous free run is an error result.
    Result<IndexRange> reserveViews(DeviceCore& core, uint32_t count);

    /// Reserves `count` contiguous slots of the sampler heap (VU-rojo-texture-0035). No contiguous free run is an error result.
    Result<IndexRange> reserveSamplers(DeviceCore& core, uint32_t count);

    /// Releases `range`, deferred to the retirement of the frame that is open, or of the next one (VU-rojo-texture-0038); the
    /// textures its slots described no longer count those views.
    void releaseRange(DeviceCore& core, IndexRange range);
} // namespace rojoRHI::core
