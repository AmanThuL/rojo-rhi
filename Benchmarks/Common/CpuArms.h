//----------------------------------------------------------------------------------------------------------------------
/// @file CpuArms.h
/// @brief Declares the arm runners of the CPU operation workloads, each defined beside its arm.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/RunOptions.h"

namespace rojoRHI::bench {
    ArmResult runRojoC5(const Config& config, const RunOptions& options);
    ArmResult runNativeC5(const Config& config, const RunOptions& options);
    ArmResult runRojoC7(const Config& config, const RunOptions& options);
    ArmResult runNativeC7(const Config& config, const RunOptions& options);
} // namespace rojoRHI::bench
