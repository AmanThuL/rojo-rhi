//----------------------------------------------------------------------------------------------------------------------
/// @file Upscalers.h
/// @brief Declares the creation of an upscaler and the checks of the upscaling rules of chapter 12, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Handles.h>
#include <rojoRHI/Result.h>
#include <rojoRHI/UpscaleParams.h>
#include <rojoRHI/UpscalerDesc.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"

namespace rojoRHI::core {
    /// Creates an upscaler: checks the descriptor (VU-rojo-upscale-0001 to 0003, VU-rojo-core-0006, VU-rojo-core-0007), resolves
    /// `Auto` to the first kind the device lists, and stores a record of the resolved description. A kind the device does not
    /// list, `Auto` on a device that lists none, is an error result, never a substitution. The Null backend has no native
    /// upscaler, so this is the whole entry.
    Result<Upscaler> createUpscaler(DeviceCore& core, const UpscalerDesc& desc);

    /// Checks one upscale (VU-rojo-cmd-0063, VU-rojo-upscale-0004 to 0015, VU-rojo-present-0012, VU-rojo-core-0001 to 0003). The
    /// operation records nothing else on the Null backend. Release builds check nothing and take no lock.
    void upscale(CommandListCore& list, Upscaler upscaler, const UpscaleParams& params);
} // namespace rojoRHI::core
