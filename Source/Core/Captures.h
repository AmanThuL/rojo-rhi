//----------------------------------------------------------------------------------------------------------------------
/// @file Captures.h
/// @brief Declares the checks and state of programmatic capture of chapter 14, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <string_view>

namespace rojoRHI::core {
    // Without a capture hook the result is `Unsupported`; only success marks a capture in progress.
    Result<void> beginCapture(DeviceCore& core, std::string_view path);

    void endCapture(DeviceCore& core);
} // namespace rojoRHI::core
