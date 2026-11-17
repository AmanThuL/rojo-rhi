//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Capture.h
/// @brief Declares the capture hook the Metal 4 backend registers with the device core.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <string_view>

namespace rojoRHI::metal4 {
    // Metal decides this from `MTL_CAPTURE_ENABLED` at launch.
    bool captureSupported();

    Result<void> startCapture(std::string_view path);
} // namespace rojoRHI::metal4
