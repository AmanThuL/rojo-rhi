//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Capture.h
/// @brief Declares the capture hook the Metal 4 backend registers with the device core.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <string_view>

namespace rojoRHI::metal4 {
    /// Starts a GPU capture of the system default Metal device to the `.gputrace` document at `path`, made absolute first.
    /// Returns `ErrorCategory::Unsupported` when the process cannot write a trace document (Metal reads `MTL_CAPTURE_ENABLED`
    /// at launch), when another capture is in progress or when the path cannot be made absolute, and
    /// `ErrorCategory::DriverFailure` with Metal's reason when Metal refuses the capture. It creates and deletes no file.
    Result<void> startCapture(std::string_view path);
} // namespace rojoRHI::metal4
