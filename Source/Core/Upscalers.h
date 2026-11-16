//----------------------------------------------------------------------------------------------------------------------
/// @file Upscalers.h
/// @brief Declares the preparation of an upscaler's record and the checks of the upscaling rules of chapter 12, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"

namespace rojoRHI::core {
    /// Prepares the record of an upscaler: checks the descriptor (VU-rojo-upscale-0001 to 0003, VU-rojo-core-0006,
    /// VU-rojo-core-0007), resolves the label and `Auto` to the first kind the device lists, and returns the record of the
    /// resolved description with no native payload. A kind the device does not list, `Auto` on a device that lists none, is
    /// an error result, never a substitution. Nothing is allocated: the backend creates its native object, stores it in
    /// `UpscalerRecord::native` and then allocates the record, so the payload is in place before the handle exists.
    Result<UpscalerRecord> prepareUpscaler(DeviceCore& core, const UpscalerDesc& desc);

    /// Checks one upscale (VU-rojo-cmd-0063, VU-rojo-upscale-0004 to 0015, VU-rojo-present-0012, VU-rojo-core-0001 to 0003). The
    /// operation forms a scope named `params.label`, or `rojorhi.upscale.unnamed`, and records nothing else on the Null backend. Release builds
    /// check nothing and take no lock.
    void upscale(CommandListCore& list, Upscaler upscaler, const UpscaleParams& params);
} // namespace rojoRHI::core
