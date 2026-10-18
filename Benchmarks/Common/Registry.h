//----------------------------------------------------------------------------------------------------------------------
/// @file Registry.h
/// @brief Declares the registry of every benchmark configuration and the runner of each of its arms.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/RunOptions.h"

#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    // Runs one arm of a configuration to completion: setup, warm-up, measured frames, readback and teardown.
    using ArmRunner = ArmResult (*)(const Config& config, const RunOptions& options);

    struct WorkloadEntry
    {
        Config config;
        ArmRunner rojo = nullptr;
        ArmRunner native = nullptr;
        ArmRunner fixed = nullptr; ///< Null when the configuration has no F arm.

        ArmRunner runner(Arm arm) const;
    };

    // A configuration with its runners; Config::arms is set from the runners that exist.
    WorkloadEntry makeEntry(Config config, ArmRunner rojo, ArmRunner native, ArmRunner fixed = nullptr);
    // Every configuration, built once from the four family registrations, in their order.
    const std::vector<WorkloadEntry>& registry();
    // The entry with this id, or null.
    const WorkloadEntry* find(std::string_view id);
} // namespace rojoRHI::bench
