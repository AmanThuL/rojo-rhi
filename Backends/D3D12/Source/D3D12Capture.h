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
    /// Loads the newest installed PIX GPU capturer, `WinPixGpuCapturer.dll` of the highest version directory under
    /// `%ProgramFiles%\Microsoft PIX`, into the process. It must run before the device factory exists. A process that already
    /// holds the capturer, or has no PIX installed, is left as it is, and a capturer that fails to load is logged as a warning.
    /// The module stays loaded until the process ends, because the capturer cannot be unloaded safely. Returns whether the
    /// capturer is loaded afterwards.
    bool loadCapturer();

    /// Returns the index of the highest version in `names`, the directory names under the PIX install directory, comparing the
    /// runs of digits in each name as numbers from left to right; the first of equal versions wins. Returns nothing for an empty
    /// list.
    std::optional<size_t> newestVersion(std::span<const std::wstring> names);

    /// Returns `text` converted from UTF-8 to UTF-16; empty when `text` is empty or is not valid UTF-8.
    std::wstring wide(std::string_view text);

    /// The capture hook of a device created with `DeviceDesc::enableCapture`: starts a programmatic GPU capture written to
    /// `path`. Returns `Unsupported` while the PIX GPU capturer is not loaded, and `DriverFailure` when the capturer refuses
    /// the capture.
    Result<void> startCapture(std::string_view path);

    /// The capture hook of a device created without `DeviceDesc::enableCapture`: returns `Unsupported` naming
    /// `enableCapture`, whether or not another device loaded the capturer into the process.
    Result<void> refuseCapture(std::string_view path);
} // namespace rojoRHI::d3d12
