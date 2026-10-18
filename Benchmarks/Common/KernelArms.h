//----------------------------------------------------------------------------------------------------------------------
/// @file KernelArms.h
/// @brief Declares the arm runners of the GPU kernel workloads, each defined beside its arm.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/RunOptions.h"

namespace rojoRHI::bench {
    ArmResult runRojoG1(const Config& config, const RunOptions& options);
    ArmResult runNativeG1(const Config& config, const RunOptions& options);
    ArmResult runRojoG2(const Config& config, const RunOptions& options);
    ArmResult runNativeG2(const Config& config, const RunOptions& options);
    ArmResult runFixedG2(const Config& config, const RunOptions& options);
} // namespace rojoRHI::bench
