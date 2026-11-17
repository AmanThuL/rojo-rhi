//----------------------------------------------------------------------------------------------------------------------
/// @file Samplers.h
/// @brief Declares the checks of the sampler rules of chapter 04, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <cstdint>

namespace rojoRHI::core {
    void checkSampler(DeviceCore& core, const SamplerDesc& desc);

    // A full heap is `OutOfMemory` and the caller keeps `native`; otherwise the slot owns it until the `sampler` retire hook.
    Result<SamplerIndex> addSampler(DeviceCore& core, void* native);

    // The slot frees when the open frame, or the next one, retires.
    void destroySampler(DeviceCore& core, SamplerIndex sampler);

    void checkSamplerWrite(DeviceCore& core, IndexRange range, uint32_t index, const SamplerDesc& desc);

    // The replaced payload goes to the `sampler` retire hook once the open frame, or the next one, retires. False when a racing
    // release took the range after the check: nothing is placed and the caller keeps `native`.
    bool placeSampler(DeviceCore& core, IndexRange range, uint32_t index, void* native);
} // namespace rojoRHI::core
