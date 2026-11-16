//----------------------------------------------------------------------------------------------------------------------
/// @file Captures.cpp
/// @brief Implements the checks and state of programmatic capture.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Captures.h"

#include "Base/Assert.h"

#include <string>

namespace rojoRHI::core {
    //==================================================================================================================
    Result<void> beginCapture(DeviceCore& core, [[maybe_unused]] std::string_view path)
    {
        ROJORHI_VU(!path.empty(), "VU-rojo-diag-0009", "beginCapture was given an empty path");
        if (core.captureAvailable() == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::Unsupported, "this backend has no capture tool" });
        }
        // Claiming the in-progress state and checking it are one step, so two concurrent calls cannot both pass.
        [[maybe_unused]] const bool wasCapturing = core.exchangeCapturing(true);
        ROJORHI_VU(!wasCapturing, "VU-rojo-diag-0008", "beginCapture was called on the device \"{}\" while its capture is in progress", core.label());
        Result<void> started = core.captureAvailable()(path);
        if (!started.has_value())
        {
            core.exchangeCapturing(false);
        }
        return started;
    }

    //==================================================================================================================
    void endCapture(DeviceCore& core)
    {
        [[maybe_unused]] const bool wasCapturing = core.exchangeCapturing(false);
        ROJORHI_VU(wasCapturing, "VU-rojo-diag-0010", "endCapture was called on the device \"{}\" with no capture in progress", core.label());
    }
} // namespace rojoRHI::core
