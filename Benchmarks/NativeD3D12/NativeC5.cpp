//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC5.cpp
/// @brief Defines C5's native-arm runner on D3D12, which reports that the arm is not built.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"

#include <format>

namespace rojoRHI::bench {
    //==================================================================================================================
    // The native arm of C5 is not built on d3d12, so a run records a failed arm N.
    ArmResult runNativeC5(const Config& config, const RunOptions& /*options*/)
    {
        ArmResult result;
        result.arm = Arm::Native;
        result.ok = false;
        result.error = std::format("{} arm N: the native arm of {} is not built on d3d12", config.id, config.workload);
        return result;
    }
} // namespace rojoRHI::bench
