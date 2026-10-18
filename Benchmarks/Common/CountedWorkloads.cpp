//----------------------------------------------------------------------------------------------------------------------
/// @file CountedWorkloads.cpp
/// @brief Registers C10 and its maxCount 1 row.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CountedArms.h"
#include "Common/CountedData.h"
#include "Common/Workloads.h"

#include <array>
#include <string>

namespace rojoRHI::bench {
    namespace {
        // Draws per pass: the sweep of C10, bounded by the indirect command buffers each counted draw owns.
        constexpr std::array<uint32_t, 4> kDrawSweep{ 1, 10, 100, 1'000 };
    } // namespace

    //==================================================================================================================
    void registerCountedWorkloads(std::vector<WorkloadEntry>& entries)
    {
        for (const uint32_t k : kDrawSweep)
        {
            const std::string suffix = "-k" + std::to_string(k);
            entries.push_back(
                makeEntry(Config{ .id = "C10" + suffix, .workload = "C10", .k = k, .maxCount = kCountedMaxCount, .nullCount = false }, runRojoC10, runNativeC10)
            );
            entries.push_back(
                makeEntry(Config{ .id = "C10m1" + suffix, .workload = "C10m1", .k = k, .maxCount = 1, .nullCount = true }, runRojoC10, runNativeC10)
            );
        }
    }
} // namespace rojoRHI::bench
