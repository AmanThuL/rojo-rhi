//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrame.cpp
/// @brief Defines the composed frame's native-arm runner on D3D12, which reports that the arm is not built.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/FrameArms.h"

#include <format>

namespace rojoRHI::bench {
    //==================================================================================================================
    // The native arm of the composed frame is not built on d3d12, so a run records a failed arm N.
    ArmResult runNativeFrame(const Config& config, const RunOptions& /*options*/)
    {
        ArmResult result;
        result.arm = Arm::Native;
        result.ok = false;
        result.error = std::format("{} arm N: the native arm of the composed frame is not built on d3d12", config.id);
        return result;
    }
} // namespace rojoRHI::bench
