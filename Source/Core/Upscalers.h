//----------------------------------------------------------------------------------------------------------------------
/// @file Upscalers.h
/// @brief Declares the preparation of an upscaler's record and the checks of the upscaling rules of chapter 12, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"

namespace rojoRHI::core {
    // Resolves `Auto` to the device's first kind; an unlisted kind is an error, never a substitution. Allocates nothing: the
    // backend stores its native object in `UpscalerRecord::native`, then allocates the record, so the payload is in place
    // before the handle exists.
    Result<UpscalerRecord> prepareUpscaler(DeviceCore& core, const UpscalerDesc& desc);

    // Forms a scope named `params.label`, or `rojorhi.upscale.unnamed`. Release checks nothing and takes no lock.
    void upscale(CommandListCore& list, Upscaler upscaler, const UpscaleParams& params);
} // namespace rojoRHI::core
