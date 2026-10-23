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
    /// The payload of an upscaler's record: the MetalFX temporal scaler, the fence that hands work to and from its encoders,
    /// and the cleared reactive mask a null `UpscaleParams::reactive` binds. Each pointer is one retained reference, released
    /// by `retireUpscaler`.
    struct UpscalerState
    {
        MTL4FX::TemporalScaler* scaler = nullptr; ///< The scaler, made with the device's compiler; its fence is `fence`.
        MTL::Fence* fence = nullptr;              ///< Updated by an upscale's carrier encoder and by MetalFX's encoders.
        MTL::Texture* mask = nullptr;             ///< An `R8Unorm` Private texture of the input extent holding zeros, in the device set.
        Extent2D input = {};                      ///< The descriptor's input extent, the content of an upscale that names none.
        /// The smallest content extent in each axis whose scale to the output the scaler takes; a smaller content is raised to it.
        Extent2D minContent = {};
    };

    /// Returns the upscalers `device` creates: `MetalFX` when MetalFX supports the device on Metal 4, else none. MetalFX's
    /// content scales are output over input, so the input scale range inverts them. The span views `storage`.
    std::span<const UpscalerSupport> metalUpscalers(MTL::Device* device, std::array<UpscalerSupport, 1>& storage);

    /// Releases the scaler, the fence and the mask of a retiring record inside an autorelease pool, after the mask leaves the
    /// device residency set, and counts the fence and the mask as released native objects. `context` is the `DeviceImpl`; a
    /// null payload is skipped. It runs under the upscaler pool's lock.
    void retireUpscaler(void* context, core::UpscalerRecord& record);
} // namespace rojoRHI::metal4
