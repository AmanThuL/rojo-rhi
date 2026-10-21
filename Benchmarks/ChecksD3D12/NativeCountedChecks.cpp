//----------------------------------------------------------------------------------------------------------------------
/// @file NativeCountedChecks.cpp
/// @brief Checks the native arm of the counted-draw workloads on D3D12: its image and one ExecuteIndirect per counted draw.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/CountedData.h"
#include "Common/Registry.h"
#include "Common/RunOptions.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI::bench;

namespace {
    //==================================================================================================================
    ArmResult runConfig(const WorkloadEntry& entry, const Config& config, const RunOptions& options)
    {
        REQUIRE(entry.runner(Arm::Native) != nullptr);
        ArmResult result = entry.runner(Arm::Native)(config, options);
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(result.nativeCalls.size() == options.measuredFrames);
        return result;
    }

    //==================================================================================================================
    ArmResult runConfiguration(std::string_view id, const RunOptions& options)
    {
        const WorkloadEntry* entry = find(id);
        REQUIRE(entry != nullptr);
        return runConfig(*entry, entry->config, options);
    }

    //==================================================================================================================
    // Native calls per counted draw by metric 2: the first measured frame's count at K 1,000 less K 100, over 900.
    double callsPerDraw(const ArmResult& atThousand, const ArmResult& atHundred)
    {
        return static_cast<double>(atThousand.nativeCalls[0] - atHundred.nativeCalls[0]) / 900.0;
    }
} // namespace

//======================================================================================================================
TEST_CASE("C10's native arm draws drawId + 1 at each drawId pixel with one ExecuteIndirect per counted draw", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    for (const uint32_t k : { 1u, 100u })
    {
        CAPTURE(k);
        const ArmResult result = runConfiguration("C10-k" + std::to_string(k), options);
        REQUIRE(result.outputs.size() == 1);
        CHECK(result.outputs[0].name == "target");
        CHECK(result.outputs[0].bytes == expectedTarget(kCountedMaxCount, kCountedMaxCount));
        // The one barrier of a frame discards the target into LAYOUT_RENDER_TARGET; the records and the count need none.
        CHECK(result.nativeBarriers == std::vector<uint64_t>(options.measuredFrames, 1));
    }
    const ArmResult hundred = runConfiguration("C10-k100", options);
    const ArmResult thousand = runConfiguration("C10-k1000", options);
    CHECK(callsPerDraw(thousand, hundred) == 1.0);
    REQUIRE(thousand.outputs.size() == 1);
    CHECK(thousand.outputs[0].bytes == expectedTarget(kCountedMaxCount, kCountedMaxCount));
}

//======================================================================================================================
TEST_CASE("C10m1's native arm draws its one point with one ExecuteIndirect and no count buffer", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    for (const std::string_view id : { "C10m1-k1", "C10m1-k100" })
    {
        CAPTURE(id);
        const ArmResult result = runConfiguration(id, options);
        REQUIRE(result.outputs.size() == 1);
        CHECK(result.outputs[0].name == "target");
        CHECK(result.outputs[0].bytes == expectedSinglePoint());
    }
    const ArmResult hundred = runConfiguration("C10m1-k100", options);
    const ArmResult thousand = runConfiguration("C10m1-k1000", options);
    CHECK(callsPerDraw(thousand, hundred) == 1.0);
}

//======================================================================================================================
TEST_CASE("the native counted-draw signature writes drawId to b1 at maxCount 256 and 10,000", "[native]")
{
    // Every record's drawId reaches the shader through b1, for the draw signature and the UInt16 indexed one, under a count
    // buffer holding maxCount.
    const WorkloadEntry* entry = find("C10-k1");
    REQUIRE(entry != nullptr);
    for (const std::string_view pattern : { "draw", "indexed" })
    {
        for (const uint32_t maxCount : { 256u, 10'000u })
        {
            CAPTURE(pattern, maxCount);
            Config config = entry->config;
            config.k = 3;
            config.maxCount = maxCount;
            config.pattern = std::string(pattern);
            const ArmResult result = runConfig(*entry, config, runOptions(Mode::Correctness));
            REQUIRE(result.outputs.size() == 1);
            CHECK(result.outputs[0].bytes == expectedTarget(maxCount, maxCount));
        }
    }
}
