//----------------------------------------------------------------------------------------------------------------------
/// @file Registry.cpp
/// @brief Implements the registry over the CPU, counted-draw, kernel and frame workload families.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/Registry.h"

#include "Common/Workloads.h"

#include <utility>

namespace rojoRHI::bench {
    namespace {
        //==============================================================================================================
        std::vector<WorkloadEntry> buildRegistry()
        {
            std::vector<WorkloadEntry> entries;
            registerCpuWorkloads(entries);
            registerCountedWorkloads(entries);
            registerKernelWorkloads(entries);
            registerFrameWorkloads(entries);
            return entries;
        }
    } // namespace

    //==================================================================================================================
    ArmRunner WorkloadEntry::runner(Arm arm) const
    {
        switch (arm)
        {
        case Arm::Rojo: return rojo;
        case Arm::Native: return native;
        case Arm::Fixed: return fixed;
        }
        return nullptr;
    }

    //==================================================================================================================
    WorkloadEntry makeEntry(Config config, ArmRunner rojo, ArmRunner native, ArmRunner fixed)
    {
        config.arms.clear();
        for (const auto& [arm, runner] : { std::pair{ Arm::Rojo, rojo }, std::pair{ Arm::Native, native }, std::pair{ Arm::Fixed, fixed } })
        {
            if (runner != nullptr)
            {
                config.arms.push_back(arm);
            }
        }
        return WorkloadEntry{ std::move(config), rojo, native, fixed };
    }

    //==================================================================================================================
    const std::vector<WorkloadEntry>& registry()
    {
        static const std::vector<WorkloadEntry> entries = buildRegistry();
        return entries;
    }

    //==================================================================================================================
    const WorkloadEntry* find(std::string_view id)
    {
        for (const WorkloadEntry& entry : registry())
        {
            if (entry.config.id == id)
            {
                return &entry;
            }
        }
        return nullptr;
    }
} // namespace rojoRHI::bench
