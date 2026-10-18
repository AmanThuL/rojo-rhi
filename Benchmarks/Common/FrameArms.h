//----------------------------------------------------------------------------------------------------------------------
/// @file FrameArms.h
/// @brief Declares the arm runners of the composed frame, each defined beside its arm.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/RunOptions.h"

namespace rojoRHI::bench {
    ArmResult runRojoFrame(const Config& config, const RunOptions& options);
    ArmResult runNativeFrame(const Config& config, const RunOptions& options);
} // namespace rojoRHI::bench
