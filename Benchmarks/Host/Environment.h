//----------------------------------------------------------------------------------------------------------------------
/// @file Environment.h
/// @brief Declares the environment object's writer, and the device and OS names a run records.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace rojoRHI::bench {
    // Writes {"device", "families", "metal4", "timestampHz", "os", "thermal"} to `path`. Without a Metal device the file
    // still holds every key, with "device" and "timestampHz" null, and the call returns an error.
    std::expected<void, std::string> writeEnvironmentFile(const std::filesystem::path& path);
    // The system Metal device's name, or "none" without one.
    std::string deviceName();
    // NSProcessInfo's operating system version string.
    std::string osVersion();
} // namespace rojoRHI::bench
