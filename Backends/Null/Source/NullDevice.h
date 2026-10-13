//----------------------------------------------------------------------------------------------------------------------
/// @file NullDevice.h
/// @brief Declares the Null backend's device state.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Device.h>

#include "Core/DeviceCore.h"

#include <cstdint>

namespace rojoRHI {
    /// The Null device's `Limits::maxAddrOffset`: the 32-bit byte offset an address carries in its `hi` word, 2^32 - 1.
    constexpr uint64_t kNullMaxAddrOffset = 4'294'967'295;

    /// The Null backend's device: the shared core, which also keeps the capabilities it reports. It renders nothing.
    struct DeviceImpl
    {
        /// Creates the state of a device from a description `core::validateDeviceDesc` accepted.
        explicit DeviceImpl(const DeviceDesc& desc) : core(desc)
        {
        }

        core::DeviceCore core; ///< Frame bookkeeping, pools, labels, capabilities and the format table.

        /// Returns the state `device` wraps; checks VU-rojo-core-0004.
        static DeviceImpl& of(Device device);

        /// Returns the device handle that wraps `impl`.
        static Device wrap(DeviceImpl* impl);
    };
} // namespace rojoRHI
