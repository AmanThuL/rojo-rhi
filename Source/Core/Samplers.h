//----------------------------------------------------------------------------------------------------------------------
/// @file Samplers.h
/// @brief Declares the checks of the sampler rules of chapter 04, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <cstdint>

namespace rojoRHI::core {
    /// Checks a sampler description (VU-rojo-texture-0029, 0030, VU-rojo-core-0006, VU-rojo-core-0007). A backend calls it
    /// first in `createSampler`, then creates its native sampler and calls `addSampler`.
    void checkSampler(DeviceCore& core, const SamplerDesc& desc);

    /// Takes a slot of the sampler heap holding `native`, the backend's sampler or null. A full heap is `OutOfMemory`,
    /// counting nothing; the caller still owns `native` and releases it. The slot's payload goes to the `sampler` retire hook
    /// when the slot retires or the device is destroyed.
    Result<SamplerIndex> addSampler(DeviceCore& core, void* native);

    /// Releases the sampler slot `sampler`, deferred to the retirement of the frame that is open, or of the next one; it was
    /// returned by `createSampler` (VU-rojo-device-0010).
    void destroySampler(DeviceCore& core, SamplerIndex sampler);

    /// Checks a write of a sampler into slot `range.base + index` of a reserved sampler range (VU-rojo-texture-0036, 0037,
    /// VU-rojo-core-0006, VU-rojo-core-0007) and that `range` is a reserved, unreleased range as returned
    /// (VU-rojo-texture-0031). A backend calls it first in `writeSampler`, then creates its native sampler and calls
    /// `placeSampler`.
    void checkSamplerWrite(DeviceCore& core, IndexRange range, uint32_t index, const SamplerDesc& desc);

    /// Places `native`, the backend's sampler or null, in slot `range.base + index`, which `checkSamplerWrite` accepted, and
    /// counts the write. The payload it replaces waits for the open frame, or the next one, and then goes to the `sampler`
    /// retire hook. Returns false, placing and counting nothing, when a racing release took the range after the check; the
    /// caller then still owns `native` and releases it.
    bool placeSampler(DeviceCore& core, IndexRange range, uint32_t index, void* native);
} // namespace rojoRHI::core
