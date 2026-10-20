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
    // Nine warm-up frames reuse each frame slot three times, past the first reuses' one-time allocations.
    RunOptions steadyOptions()
    {
        RunOptions options = countOptions();
        options.warmupFrames = 9;
        return options;
    }

    //==================================================================================================================
    // The result of one run of `workload` at `k` by `arm` with `options`, the count options by default.
    ArmResult countRun(std::string_view workload, uint32_t k, Arm arm, const RunOptions& options = countOptions())
    {
        const std::string id = std::string(workload) + "-k" + std::to_string(k);
        const WorkloadEntry* entry = find(id);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->runner(arm) != nullptr);
        ArmResult result = entry->runner(arm)(entry->config, options);
        INFO(result.error);
        REQUIRE(result.ok);
        return result;
    }

    //==================================================================================================================
    // The first measured frame's native calls: for R the delta of nativeRecordCalls, nativeSubmitCalls and
    // nativeHostCalls from Device::counters, for N its hand counts.
    uint64_t frameCalls(std::string_view workload, uint32_t k, Arm arm)
    {
        const ArmResult result = countRun(workload, k, arm);
        REQUIRE(!result.nativeCalls.empty());
        return result.nativeCalls[0];
    }

    //==================================================================================================================
    // The allocations of every thread during the first measured frame, with `options`.
    uint64_t frameAllocations(std::string_view workload, uint32_t k, Arm arm, const RunOptions& options = countOptions())
    {
        const ArmResult result = countRun(workload, k, arm, options);
        REQUIRE(!result.allocations.empty());
        return result.allocations[0];
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

//======================================================================================================================
TEST_CASE("a C8 render pass allocates less on Metal 4 than the native arm's, which times it on the command buffer", "[metal4]")
{
    // Per pass, from the first measured frame at K 100 and 1,000. N writes the pass's two timestamps with
    // writeTimestampIntoHeap on the command buffer, which allocates inside Metal; R writes them on the pass's encoders, which
    // allocate nothing. The pair costs N one block without the validation layer (R 6.01, N 7.01 in Debug) and many with it (R 25.4,
    // N 40.6), so R stays at least one allocation below N in every build and layer setting.
    const auto perPass = [](Arm arm) {
        return (static_cast<double>(frameAllocations("C8", 1000, arm)) - static_cast<double>(frameAllocations("C8", 100, arm))) / 900.0;
    };
    const double rojo = perPass(Arm::Rojo);
    const double native = perPass(Arm::Native);
    CAPTURE(rojo, native);
    CHECK(rojo <= native - 0.9);
}

//======================================================================================================================
TEST_CASE("C4 to C7 allocate at most two blocks a frame more on Metal 4 than the native arm: the frame event's signal", "[metal4]")
{
    // From the first measured frame at K 100, after nine warm-up frames. Both arms' commits allocate alike; R adds the frame event's signalEvent, two
    // blocks inside Metal (2026-10-20 probe), and nothing else: none of these workloads draws counted, so no list waits on or
    // updates the carry fence, each of which would allocate one more.
    for (const std::string_view workload : { "C4", "C5", "C6", "C7" })
    {
        DYNAMIC_SECTION(workload)
        {
            const uint64_t rojo = frameAllocations(workload, 100, Arm::Rojo, steadyOptions());
            const uint64_t native = frameAllocations(workload, 100, Arm::Native, steadyOptions());
            CAPTURE(rojo, native);
            CHECK(rojo <= native + 2);
        }
    }
}
