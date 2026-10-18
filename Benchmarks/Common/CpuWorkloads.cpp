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
        // The repetitions per frame of C1 to C7.
        constexpr std::array<uint32_t, 6> kOperationSweep{ 1, 10, 100, 1'000, 10'000, 100'000 };
        // C8's passes per frame: one Metal 4 command buffer aborts the process between 95,000 and 100,000 render
        // encoders, and two timestamps per pass at 100,000 passes exceed the 32 counter heaps Metal allows a process.
        constexpr std::array<uint32_t, 5> kPassSweep{ 1, 10, 100, 1'000, 10'000 };
        // The submissions per frame of C9, each of its own pre-recorded list.
        constexpr std::array<uint32_t, 3> kSubmitSweep{ 1, 10, 100 };

        // One CPU operation and its arms' runners.
        struct CpuWorkload
        {
            std::string_view name;
            ArmRunner rojo = nullptr;
            ArmRunner native = nullptr;
        };

        constexpr std::array<CpuWorkload, 7> kSweptWorkloads{
            CpuWorkload{ "C1", runRojoC1, runNativeC1 }, CpuWorkload{ "C2", runRojoC2, runNativeC2 }, CpuWorkload{ "C3", runRojoC3, runNativeC3 },
            CpuWorkload{ "C4", runRojoC4, runNativeC4 }, CpuWorkload{ "C5", runRojoC5, runNativeC5 }, CpuWorkload{ "C6", runRojoC6, runNativeC6 },
            CpuWorkload{ "C7", runRojoC7, runNativeC7 },
        };

        //==============================================================================================================
        Config cpuConfig(std::string_view workload, uint32_t k)
        {
            return Config{ .id = std::format("{}-k{}", workload, k), .workload = std::string(workload), .k = k };
        }
    } // namespace

    //==================================================================================================================
    void registerCpuWorkloads(std::vector<WorkloadEntry>& entries)
    {
        for (const CpuWorkload& workload : kSweptWorkloads)
        {
            for (const uint32_t k : kOperationSweep)
            {
                entries.push_back(makeEntry(cpuConfig(workload.name, k), workload.rojo, workload.native));
            }
        }
        for (const uint32_t k : kPassSweep)
        {
            entries.push_back(makeEntry(cpuConfig("C8", k), runRojoC8, runNativeC8));
        }
        for (const uint32_t k : kSubmitSweep)
        {
            entries.push_back(makeEntry(cpuConfig("C9", k), runRojoC9, runNativeC9));
        }
    }
} // namespace rojoRHI::bench
