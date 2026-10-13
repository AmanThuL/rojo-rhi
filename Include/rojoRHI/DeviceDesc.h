//----------------------------------------------------------------------------------------------------------------------
/// @file DeviceDesc.h
/// @brief Declares the description of a device to create.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace rojoRHI {
    /// Describes the device `createDevice` opens.
    struct DeviceDesc
    {
        /// Runs the validation tracker for the device's lifetime. The tracker exists only in non-Release builds; with this set a
        /// Release build's `createDevice` returns `ErrorCategory::Unsupported`.
        bool validation = false;
        /// Prepares programmatic GPU capture before the device exists, on backends whose capture tool must be loaded first.
        bool enableCapture = false;
        /// Frame slots, which bound how many frames the CPU records ahead of the GPU: 2 or 3 (VU-rojo-device-0001).
        uint32_t framesInFlight = 3;
        /// A blob from `Device::pipelineCacheData`, read during `createDevice`; empty for none. A blob from another driver,
        /// device or RojoRHI version is ignored.
        std::span<const std::byte> pipelineCache;
        /// Names the device in captures and messages; "rojorhi.device.unnamed" when empty.
        std::string_view label;
    };
} // namespace rojoRHI
