//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Upscaler.cpp
/// @brief Implements the public upscaler entries for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

namespace rojoRHI {

    //==================================================================================================================
    Result<Upscaler> Device::createUpscaler([[maybe_unused]] const UpscalerDesc& desc)
    {
        DeviceImpl::of(*this);
        return std::unexpected(Error{ ErrorCategory::Unsupported, "Device::createUpscaler is not supported by the Metal 4 backend" });
    }

    //==================================================================================================================
    void Device::destroy(Upscaler upscaler)
    {
        DeviceImpl::of(*this).core.destroy(upscaler);
    }
} // namespace rojoRHI
