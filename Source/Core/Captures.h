//----------------------------------------------------------------------------------------------------------------------
/// @file Captures.h
/// @brief Declares the checks and state of programmatic capture of chapter 14, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <string_view>

namespace rojoRHI::core {
    /// Starts a capture to `path`: VU-rojo-diag-0009 (an empty path), then VU-rojo-diag-0008 (this device is capturing), then the
    /// backend's availability hook. Without a hook there is no capture tool and the result is `ErrorCategory::Unsupported`; a
    /// refusal by the hook is returned as it is. Only success marks a capture in progress.
    Result<void> beginCapture(DeviceCore& core, std::string_view path);

    /// Ends the capture in progress (VU-rojo-diag-0010).
    void endCapture(DeviceCore& core);
} // namespace rojoRHI::core
