//----------------------------------------------------------------------------------------------------------------------
/// @file ThermalState.h
/// @brief Declares the reader of the process's thermal state, named as the run results record it.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <string_view>

namespace rojoRHI::bench {
    // One of nominal, fair, serious and critical from NSProcessInfo on macOS; unknown on Windows, which reports none.
    std::string_view thermalState();
} // namespace rojoRHI::bench
