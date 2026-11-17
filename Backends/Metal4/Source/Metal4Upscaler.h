//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Upscaler.h
/// @brief Declares the native state of a Metal 4 upscaler, a MetalFX temporal scaler, and its retire hook.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"

#include <Metal/Metal.hpp>
#include <MetalFX/MetalFX.hpp>

#include <array>
#include <span>

namespace rojoRHI::metal4 {
    // Each pointer is one retained reference, released by `retireUpscaler`.
    struct UpscalerState
    {
        MTL4FX::TemporalScaler* scaler = nullptr;
        MTL::Fence* fence = nullptr;  // Updated by an upscale's carrier encoder and by MetalFX's encoders.
        MTL::Texture* mask = nullptr; // Zeros of the input extent, bound when `UpscaleParams::reactive` is null; in the device set.
        Extent2D input = {};          // The content of an upscale that names none.
        // The smallest content extent in each axis whose scale to the output the scaler takes; a smaller content is raised to it.
        Extent2D minContent = {};
    };

    // MetalFX's content scales are output over input, so the input scale range inverts them. The span views `storage`.
    std::span<const UpscalerSupport> metalUpscalers(MTL::Device* device, std::array<UpscalerSupport, 1>& storage);

    // Releases inside an autorelease pool, after the mask leaves the device residency set. Runs under the upscaler
    // pool's lock.
    void retireUpscaler(void* context, core::UpscalerRecord& record);
} // namespace rojoRHI::metal4
