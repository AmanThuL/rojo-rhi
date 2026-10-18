//----------------------------------------------------------------------------------------------------------------------
/// @file CountedArms.h
/// @brief Declares the arm runners of the counted-draw workloads, each defined beside its arm.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/RunOptions.h"

namespace rojoRHI::bench {
    ArmResult runRojoC10(const Config& config, const RunOptions& options);
    ArmResult runNativeC10(const Config& config, const RunOptions& options);
} // namespace rojoRHI::bench
