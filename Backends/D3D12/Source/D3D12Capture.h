//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Capture.h
/// @brief Declares the PIX GPU capturer's loading and the capture hook the D3D12 backend registers with the core.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rojoRHI::d3d12 {
    // Loads `WinPixGpuCapturer.dll` of the highest version directory under `%ProgramFiles%\Microsoft PIX`. It must run
    // before the device factory exists. A process that already holds the capturer, or has no PIX installed, is left as
    // it is; a failed load is a warning. The module is never unloaded: the capturer cannot be unloaded safely.
    bool loadCapturer();

    // Compares the runs of digits in each name as numbers, left to right; the first of equal versions wins.
    std::optional<size_t> newestVersion(std::span<const std::wstring> names);

    // Empty when `text` is empty or is not valid UTF-8.
    std::wstring wide(std::string_view text);

    // The hook of a device created with `enableCapture` once the capturer loaded: `Unsupported` when the capturer lacks
    // the export, `DriverFailure` when it refuses the capture.
    Result<void> startCapture(std::string_view path);

    // The hook of a device created without `DeviceDesc::enableCapture`: refuses even when another device loaded the
    // capturer into the process.
    Result<void> refuseCapture(std::string_view path);

    // The hook of a device created with `enableCapture` while the capturer did not load: refuses naming PIX, so a
    // device whose `Capabilities::capture` is false never starts a capture.
    Result<void> refuseWithoutCapturer(std::string_view path);
} // namespace rojoRHI::d3d12
