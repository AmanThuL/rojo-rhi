//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Counts.cpp
/// @brief Checks RojoRHI's native calls per CPU operation against the native arm's on the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/Registry.h"
#include "Common/RunOptions.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

using namespace rojoRHI::bench;

namespace {
    // One CPU operation: its metric 2 K pair, its RojoRHI calls per repetition c and the native calls per repetition
    // the note registers for N.
    struct CountRow
    {
        std::string_view workload;
        uint32_t low = 100;
        uint32_t high = 1000;
        uint32_t c = 0;
        uint32_t native = 0;
    };

    constexpr std::array<CountRow, 9> kCountRows{
        CountRow{ .workload = "C1", .c = 3, .native = 2 },
        CountRow{ .workload = "C2", .c = 3, .native = 2 },
        CountRow{ .workload = "C3", .c = 2, .native = 4 },
        CountRow{ .workload = "C4", .c = 3, .native = 2 },
        CountRow{ .workload = "C5", .c = 1, .native = 0 },
        CountRow{ .workload = "C6", .c = 2, .native = 2 },
        CountRow{ .workload = "C7", .c = 1, .native = 1 },
        CountRow{ .workload = "C8", .c = 7, .native = 12 },
        CountRow{ .workload = "C9", .low = 10, .high = 100, .c = 1, .native = 2 },
    };

    //==================================================================================================================
    // Three warm-up frames use each frame slot once, so the measured frames reuse slots, as a timing run's do.
    RunOptions countOptions()
    {
        RunOptions options = runOptions(Mode::Correctness);
        options.warmupFrames = 3;
        options.measuredFrames = 2;
        return options;
    }

    //==================================================================================================================
    // The first measured frame's native calls: for R the delta of nativeRecordCalls, nativeSubmitCalls and
    // nativeHostCalls from Device::counters, for N its hand counts.
    uint64_t frameCalls(std::string_view workload, uint32_t k, Arm arm)
    {
        const std::string id = std::string(workload) + "-k" + std::to_string(k);
        const WorkloadEntry* entry = find(id);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->runner(arm) != nullptr);
        const ArmResult result = entry->runner(arm)(entry->config, countOptions());
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(!result.nativeCalls.empty());
        return result.nativeCalls[0];
    }
} // namespace

//======================================================================================================================
TEST_CASE("every CPU operation makes at most the native arm's calls plus one per RojoRHI call on Metal 4", "[metal4]")
{
    for (const CountRow& row : kCountRows)
    {
        // One section per row, so a failing row stops only its own section.
        DYNAMIC_SECTION(row.workload)
        {
            const double span = static_cast<double>(row.high - row.low);
            const double rojo =
                (static_cast<double>(frameCalls(row.workload, row.high, Arm::Rojo)) - static_cast<double>(frameCalls(row.workload, row.low, Arm::Rojo))) / span;
            const double native =
                (static_cast<double>(frameCalls(row.workload, row.high, Arm::Native)) - static_cast<double>(frameCalls(row.workload, row.low, Arm::Native))) /
                span;
            CAPTURE(rojo, native);
            CHECK(native == static_cast<double>(row.native));
            CHECK(rojo <= native + row.c);
        }
    }
}
