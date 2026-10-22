//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12GpuSpanChecks.cpp
/// @brief Checks that both arms record a GPU span for every measured frame on the D3D12 backend.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/Registry.h"
#include "Common/RunOptions.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <optional>

using namespace rojoRHI::bench;

//======================================================================================================================
TEST_CASE("RojoRHI's and the native arm's frames on D3D12 each record a GPU span", "[d3d12]")
{
    // R's span comes from the backend's submission timing seam, which RojoGpuTiming attaches; N's from its own
    // timestamps. A frame without one would leave the round's GPU metric empty.
    const WorkloadEntry* entry = find("C1-k10");
    REQUIRE(entry != nullptr);
    RunOptions options = runOptions(Mode::Correctness);
    options.warmupFrames = 1;
    options.measuredFrames = 3;
    for (const Arm arm : { Arm::Rojo, Arm::Native })
    {
        CAPTURE(arm == Arm::Rojo ? "R" : "N");
        REQUIRE(entry->runner(arm) != nullptr);
        const ArmResult result = entry->runner(arm)(entry->config, options);
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(result.gpuNs.size() == options.measuredFrames);
        for (const std::optional<double>& gpuNs : result.gpuNs)
        {
            REQUIRE(gpuNs.has_value());
            CHECK(*gpuNs >= 0.0);
        }
    }
}
