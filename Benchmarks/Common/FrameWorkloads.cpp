//----------------------------------------------------------------------------------------------------------------------
/// @file FrameWorkloads.cpp
/// @brief Registers the composed frame.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/FrameArms.h"
#include "Common/Workloads.h"

namespace rojoRHI::bench {
    //==================================================================================================================
    void registerFrameWorkloads(std::vector<WorkloadEntry>& entries)
    {
        entries.push_back(makeEntry(Config{ .id = "frame", .workload = "frame" }, runRojoFrame, runNativeFrame));
    }
} // namespace rojoRHI::bench
