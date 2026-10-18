//----------------------------------------------------------------------------------------------------------------------
/// @file Results.h
/// @brief Declares the JSON object `RojoRHIBench run` writes for one configuration, and the comparison of two arms.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/RunOptions.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rojoRHI::bench {
    // One process's run of one configuration, with its arms in the order they ran.
    struct RunRecord
    {
        const Config& config;
        Mode mode = Mode::Correctness;
        bool validation = false;
        std::string_view device;
        std::string_view os;
        std::string_view thermalStart;
        std::string_view thermalEnd;
        std::span<const ArmResult> arms;
    };

    // The run JSON, schema 1, with no trailing newline. The build and backend are this executable's own.
    std::string runJson(const RunRecord& record);
    // Whether both arms ran, succeeded and wrote identical outputs; empty when either is absent or failed.
    std::optional<bool> outputsEqual(std::span<const ArmResult> arms, Arm first, Arm second);
} // namespace rojoRHI::bench
