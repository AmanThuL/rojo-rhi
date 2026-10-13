//----------------------------------------------------------------------------------------------------------------------
/// @file NullDevice.h
/// @brief Declares the Null backend's device state.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Capabilities.h>
#include <rojoRHI/Device.h>

#include "Core/DeviceCore.h"
#include "Core/Formats.h"

namespace rojoRHI {
    /// The Null backend's device: the shared core plus the capabilities it reports. It renders nothing.
    struct DeviceImpl
    {
        /// Creates the state of a device from a description `core::validateDeviceDesc` accepted.
        explicit DeviceImpl(const DeviceDesc& desc) : core(desc)
        {
        }

        core::DeviceCore core;     ///< Frame bookkeeping, pools and labels.
        Capabilities capabilities; ///< Fixed at creation; its views point at static storage.
        core::FormatTable formats; ///< What each format supports, fixed at creation.

        /// Returns the state `device` wraps; checks VU-rojo-core-0004.
        static DeviceImpl& of(Device device);

        /// Returns the device handle that wraps `impl`.
        static Device wrap(DeviceImpl* impl);
    };
} // namespace rojoRHI
