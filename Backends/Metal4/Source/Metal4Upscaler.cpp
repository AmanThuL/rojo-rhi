//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Upscaler.cpp
/// @brief Implements the public upscaler entries for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Upscalers.h"

namespace rojoRHI {

    //==================================================================================================================
    Result<Upscaler> Device::createUpscaler(const UpscalerDesc& desc)
    {
        // The descriptor is checked and the kind resolved first, so misuse is caught even though no scaler is made.
        const Result<core::UpscalerRecord> record = core::prepareUpscaler(DeviceImpl::of(*this).core, desc);
        if (!record)
        {
            return std::unexpected(record.error());
        }
        return std::unexpected(Error{ ErrorCategory::Unsupported, "Device::createUpscaler is not supported by the Metal 4 backend" });
    }

    //==================================================================================================================
    void Device::destroy(Upscaler upscaler)
    {
        DeviceImpl::of(*this).core.destroy(upscaler);
    }
} // namespace rojoRHI
