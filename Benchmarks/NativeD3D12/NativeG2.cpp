//----------------------------------------------------------------------------------------------------------------------
/// @file NativeG2.cpp
/// @brief Defines G2's native-arm runners on D3D12, arms N and F, which report that the arms are not built.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/KernelArms.h"

#include <format>

namespace rojoRHI::bench {
    //==================================================================================================================
    // The native arm of G2 is not built on d3d12, so a run records a failed arm N.
    ArmResult runNativeG2(const Config& config, const RunOptions& /*options*/)
    {
        ArmResult result;
        result.arm = Arm::Native;
        result.ok = false;
        result.error = std::format("{} arm N: the native arm of {} is not built on d3d12", config.id, config.workload);
        return result;
    }

    //==================================================================================================================
    // The fixed-binding native arm of G2 is not built on d3d12, so a run records a failed arm F.
    ArmResult runFixedG2(const Config& config, const RunOptions& /*options*/)
    {
        ArmResult result;
        result.arm = Arm::Fixed;
        result.ok = false;
        result.error = std::format("{} arm F: the fixed-binding native arm of {} is not built on d3d12", config.id, config.workload);
        return result;
    }
} // namespace rojoRHI::bench
