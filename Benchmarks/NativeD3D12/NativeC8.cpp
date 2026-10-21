//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC8.cpp
/// @brief Defines C8's native-arm runner on D3D12, which reports that the arm is not built.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"

#include <format>

namespace rojoRHI::bench {
    //==================================================================================================================
    // The native arm of C8 is not built on d3d12, so a run records a failed arm N.
    ArmResult runNativeC8(const Config& config, const RunOptions& /*options*/)
    {
        ArmResult result;
        result.arm = Arm::Native;
        result.ok = false;
        result.error = std::format("{} arm N: the native arm of {} is not built on d3d12", config.id, config.workload);
        return result;
    }
} // namespace rojoRHI::bench
