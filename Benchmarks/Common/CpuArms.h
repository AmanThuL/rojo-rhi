//----------------------------------------------------------------------------------------------------------------------
/// @file CpuArms.h
/// @brief Declares the arm runners of the CPU operation workloads, each defined beside its arm.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/RunOptions.h"

namespace rojoRHI::bench {
    ArmResult runRojoC1(const Config& config, const RunOptions& options);
    ArmResult runNativeC1(const Config& config, const RunOptions& options);
    ArmResult runRojoC2(const Config& config, const RunOptions& options);
    ArmResult runNativeC2(const Config& config, const RunOptions& options);
    ArmResult runRojoC3(const Config& config, const RunOptions& options);
    ArmResult runNativeC3(const Config& config, const RunOptions& options);
    ArmResult runRojoC4(const Config& config, const RunOptions& options);
    ArmResult runNativeC4(const Config& config, const RunOptions& options);
    ArmResult runRojoC5(const Config& config, const RunOptions& options);
    ArmResult runNativeC5(const Config& config, const RunOptions& options);
    ArmResult runRojoC6(const Config& config, const RunOptions& options);
    ArmResult runNativeC6(const Config& config, const RunOptions& options);
    ArmResult runRojoC7(const Config& config, const RunOptions& options);
    ArmResult runNativeC7(const Config& config, const RunOptions& options);
    ArmResult runRojoC8(const Config& config, const RunOptions& options);
    ArmResult runNativeC8(const Config& config, const RunOptions& options);
    ArmResult runRojoC9(const Config& config, const RunOptions& options);
    ArmResult runNativeC9(const Config& config, const RunOptions& options);
} // namespace rojoRHI::bench
