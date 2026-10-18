//----------------------------------------------------------------------------------------------------------------------
/// @file Samplers.h
/// @brief Declares the checks of the sampler rules of chapter 04, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <cstdint>

namespace rojoRHI::core {
    /// Creates a sampler: checks the descriptor (VU-rojo-texture-0029, 0030, VU-rojo-core-0006, VU-rojo-core-0007) and takes a
    /// slot of the sampler heap. A full heap is an error result. The Null backend has no native sampler, so this is the
    /// whole entry.
    Result<SamplerIndex> createSampler(DeviceCore& core, const SamplerDesc& desc);

    /// Releases the sampler slot `sampler`, deferred to the retirement of the frame that is open, or of the next one; it was
    /// returned by `createSampler` (VU-rojo-device-0010).
    void destroySampler(DeviceCore& core, SamplerIndex sampler);

    /// Checks a write of a sampler into slot `range.base + index` of a reserved sampler range (VU-rojo-texture-0036, 0037,
    /// VU-rojo-core-0006, VU-rojo-core-0007) and that `range` is a reserved, unreleased range as returned (VU-rojo-texture-0031). A backend calls it first in
    /// `writeSampler`; the Null backend has nothing more to do.
    void checkSamplerWrite(DeviceCore& core, IndexRange range, uint32_t index, const SamplerDesc& desc);
} // namespace rojoRHI::core
