//----------------------------------------------------------------------------------------------------------------------
/// @file ThermalState.cpp
/// @brief Implements the thermal state reader on Windows, which has no process thermal state to read.
//----------------------------------------------------------------------------------------------------------------------
#include "Host/ThermalState.h"

namespace rojoRHI::bench {
    //==================================================================================================================
    // Windows reports no thermal state to a process, so every run records "unknown" and the driver judges heat by its
    // own rule.
    std::string_view thermalState()
    {
        return "unknown";
    }
} // namespace rojoRHI::bench
