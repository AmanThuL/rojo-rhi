//----------------------------------------------------------------------------------------------------------------------
/// @file Environment.h
/// @brief Declares the environment object's writer, and the device and OS names a run records.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace rojoRHI::bench {
    // Writes the environment object to `path`. On macOS its keys are {"device", "families", "metal4", "timestampHz", "os",
    // "thermal"}; on Windows {"device", "adapter", "vendorId", "deviceId", "driverVersion", "featureLevel", "shaderModel",
    // "agility", "warp", "timestampHz", "os", "thermal"}, from the adapter the native arm selects. Without a device the
    // file still holds every key, the device's own values null, and the call returns an error.
    std::expected<void, std::string> writeEnvironmentFile(const std::filesystem::path& path);
    // The system Metal device's name on macOS, the description of the native arm's adapter on Windows; "none" without one.
    std::string deviceName();
    // NSProcessInfo's operating system version string on macOS; "Windows <major>.<minor>.<build>" from RtlGetVersion.
    std::string osVersion();
} // namespace rojoRHI::bench
