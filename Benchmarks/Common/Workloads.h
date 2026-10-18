//----------------------------------------------------------------------------------------------------------------------
/// @file Workloads.h
/// @brief Declares the registration function of each workload family, each defined in its family's own file.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/Registry.h"

#include <vector>

namespace rojoRHI::bench {
    // C1 to C9, the CPU operations.
    void registerCpuWorkloads(std::vector<WorkloadEntry>& entries);
    // C10 and its maxCount 1 row.
    void registerCountedWorkloads(std::vector<WorkloadEntry>& entries);
    // G1 and G2.
    void registerKernelWorkloads(std::vector<WorkloadEntry>& entries);
    // The composed frame.
    void registerFrameWorkloads(std::vector<WorkloadEntry>& entries);
} // namespace rojoRHI::bench
