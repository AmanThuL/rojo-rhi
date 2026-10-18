//----------------------------------------------------------------------------------------------------------------------
/// @file CpuWorkloads.cpp
/// @brief Registers the CPU operation workloads at every K of their sweep.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/Workloads.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace rojoRHI::bench {
    namespace {
        // The repetitions per frame of C1 to C8.
        constexpr std::array<uint32_t, 6> kOperationSweep{ 1, 10, 100, 1'000, 10'000, 100'000 };

        //==============================================================================================================
        Config cpuConfig(std::string_view workload, uint32_t k)
        {
            return Config{ .id = std::format("{}-k{}", workload, k), .workload = std::string(workload), .k = k };
        }
    } // namespace

    //==================================================================================================================
    void registerCpuWorkloads(std::vector<WorkloadEntry>& entries)
    {
        for (const uint32_t k : kOperationSweep)
        {
            entries.push_back(makeEntry(cpuConfig("C5", k), runRojoC5, runNativeC5));
        }
        for (const uint32_t k : kOperationSweep)
        {
            entries.push_back(makeEntry(cpuConfig("C7", k), runRojoC7, runNativeC7));
        }
    }
} // namespace rojoRHI::bench
