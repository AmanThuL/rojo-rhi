//----------------------------------------------------------------------------------------------------------------------
/// @file CountedChecks.cpp
/// @brief Checks the counted-draw workloads: RojoRHI's counts and calls on Null and the native arm's image on this device.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/CountedData.h"
#include "Common/CounterFields.h"
#include "Common/Registry.h"
#include "Common/RunOptions.h"

#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI::bench;
using rojoRHI::Counters;

#if defined(ROJORHI_BENCH_NATIVE_METAL)
namespace {
    //==================================================================================================================
    ArmResult runConfig(const WorkloadEntry& entry, const Config& config, Arm arm, const RunOptions& options)
    {
        REQUIRE(entry.runner(arm) != nullptr);
        ArmResult result = entry.runner(arm)(config, options);
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(result.nativeCalls.size() == options.measuredFrames);
        return result;
    }

    //==================================================================================================================
    ArmResult runConfiguration(std::string_view id, Arm arm, const RunOptions& options)
    {
        const WorkloadEntry* entry = find(id);
        REQUIRE(entry != nullptr);
        return runConfig(*entry, entry->config, arm, options);
    }

    //==================================================================================================================
    // Native calls per counted draw by metric 2: the first measured frame's count at K 1,000 less K 100, over 900.
    double callsPerDraw(const ArmResult& atThousand, const ArmResult& atHundred)
    {
        return static_cast<double>(atThousand.nativeCalls[0] - atHundred.nativeCalls[0]) / 900.0;
    }
} // namespace
#endif

#if defined(ROJORHI_BENCH_BACKEND_NULL)
namespace {
    using rojoRHI::MessageSeverity;

    //==================================================================================================================
    // Three warm-up frames open each of the three frame slots once, so the first measured frame reuses a slot whose list
    // pool and arena pages already exist.
    RunOptions nullOptions(bool validation, bool trace)
    {
        RunOptions options = runOptions(Mode::Correctness);
        options.warmupFrames = 3;
        options.measuredFrames = 2;
        options.validation = validation;
        options.trace = trace;
        return options;
    }

    //==================================================================================================================
    // A workload at a K the sweep does not register, through the runner of its K 1 configuration.
    ArmResult runAtK(std::string_view workload, uint32_t k, Arm arm, const RunOptions& options)
    {
        const WorkloadEntry* entry = find(std::string(workload) + "-k1");
        REQUIRE(entry != nullptr);
        Config config = entry->config;
        config.id = std::string(workload) + "-k" + std::to_string(k);
        config.k = k;
        return runConfig(*entry, config, arm, options);
    }

    //==================================================================================================================
    void checkEveryCounter(const Counters& measured, const Counters& expected)
    {
        for (const CounterField& field : kCounterFields)
        {
            CAPTURE(field.name);
            CHECK(measured.*field.member == expected.*field.member);
        }
    }

    //==================================================================================================================
    // A counted-draw frame on Null: the barrier that discards the target into ColorAttachment (one texture barrier, one
    // native barrier command, one record call), the pass (begin, end, pipeline, viewport and scissor: five record calls,
    // one encoder) and K indirect draws of one record call each. The records and the count were written at creation, so
    // no frame data is uploaded; one submit; a reused slot grows nothing.
    Counters countedFrame(uint64_t k)
    {
        return Counters{ .nativeRecordCalls = k + 6, .nativeSubmitCalls = 1, .textureBarriers = 1, .nativeBarriers = 1, .encodersOpened = 1 };
    }

    // Collects every message RojoRHI emits while a validating run is active; the default sink would print them.
    struct MessageLog
    {
        std::vector<std::string> lines;
    };

    //==================================================================================================================
    void collectMessage(MessageSeverity /*severity*/, std::string_view message, void* user)
    {
        static_cast<MessageLog*>(user)->lines.emplace_back(message);
    }

    //==================================================================================================================
    std::vector<std::string_view> frameCalls(uint32_t k)
    {
        std::vector<std::string_view> calls{ "beginFrame", "acquireCommandList", "barrier", "beginRenderPass", "setPipeline", "setViewport", "setScissor" };
        calls.insert(calls.end(), k, "drawIndirect");
        calls.insert(calls.end(), { "endRenderPass", "end", "submit", "endFrame" });
        return calls;
    }
} // namespace

//======================================================================================================================
TEST_CASE("C10 and C10m1 through RojoRHI change every Null counter of a measured frame by their documented count", "[null]")
{
    for (const std::string_view workload : { "C10", "C10m1" })
    {
        for (const uint64_t k : { 100u, 1000u })
        {
            const std::string id = std::string(workload) + "-k" + std::to_string(k);
            CAPTURE(id);
            const ArmResult result = runConfiguration(id, Arm::Rojo, nullOptions(false, false));
            REQUIRE(result.frameCounters.has_value());
            checkEveryCounter(*result.frameCounters, countedFrame(k));
            CHECK(result.nativeCalls == std::vector<uint64_t>(2, k + 7));
            CHECK(result.nativeBarriers == std::vector<uint64_t>(2, 1));
            CHECK(*result.cpuAllocations == std::vector<uint64_t>(2, 0));
        }
    }
}

//======================================================================================================================
TEST_CASE("C10 and C10m1 through RojoRHI make one native call per counted draw on Null", "[null]")
{
    for (const std::string_view workload : { "C10", "C10m1" })
    {
        const ArmResult hundred = runConfiguration(std::string(workload) + "-k100", Arm::Rojo, nullOptions(false, false));
        const ArmResult thousand = runConfiguration(std::string(workload) + "-k1000", Arm::Rojo, nullOptions(false, false));
        CHECK(callsPerDraw(thousand, hundred) == 1.0);
        const Counters perDraw = counterDelta(*thousand.frameCounters, *hundred.frameCounters);
        CHECK(perDraw.nativeRecordCalls == 900);
        CHECK(perDraw.frameDataUploads == 0);
        CHECK(perDraw.nativeBarriers == 0);
    }
}

//======================================================================================================================
TEST_CASE("one measured frame of C10 and C10m1 through RojoRHI makes exactly the registered calls in order", "[null]")
{
    for (const std::string_view workload : { "C10", "C10m1" })
    {
        for (const uint32_t k : { 1u, 2u })
        {
            CAPTURE(workload, k);
            const ArmResult result = runAtK(workload, k, Arm::Rojo, nullOptions(false, true));
            CHECK(result.trace == frameCalls(k));
        }
    }
}

//======================================================================================================================
TEST_CASE("C10 and C10m1 through RojoRHI run on a validating Null device without a message", "[null]")
{
#if defined(NDEBUG)
    SKIP("DeviceDesc::validation exists in Debug builds only");
#endif
    MessageLog log;
    rojoRHI::setMessageCallback(collectMessage, &log);
    for (const std::string_view id : { "C10-k100", "C10-k1000", "C10m1-k100", "C10m1-k1000" })
    {
        CAPTURE(id);
        const ArmResult result = runConfiguration(id, Arm::Rojo, nullOptions(true, false));
        checkEveryCounter(*result.frameCounters, countedFrame(std::stoull(std::string(id.substr(id.find("-k") + 2)))));
    }
    rojoRHI::setMessageCallback(nullptr, nullptr);
    CHECK(log.lines.empty());
}
#endif

#if defined(ROJORHI_BENCH_NATIVE_METAL)
//======================================================================================================================
TEST_CASE("C10's native arm draws drawId + 1 at each drawId pixel from kernel-encoded commands", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    for (const uint32_t k : { 1u, 100u })
    {
        CAPTURE(k);
        const ArmResult result = runConfiguration("C10-k" + std::to_string(k), Arm::Native, options);
        REQUIRE(result.outputs.size() == 1);
        CHECK(result.outputs[0].name == "target");
        CHECK(result.outputs[0].bytes == expectedTarget(kCountedMaxCount, kCountedMaxCount));
        CHECK(result.gpuNs[0].has_value());
    }
}

//======================================================================================================================
TEST_CASE("C10's native arm makes three native calls per counted draw and one barrier", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult hundred = runConfiguration("C10-k100", Arm::Native, options);
    const ArmResult thousand = runConfiguration("C10-k1000", Arm::Native, options);
    CHECK(callsPerDraw(thousand, hundred) == 3.0);
    CHECK(hundred.nativeBarriers == std::vector<uint64_t>(options.measuredFrames, 1));
    CHECK(thousand.nativeBarriers == std::vector<uint64_t>(options.measuredFrames, 1));
    REQUIRE(thousand.outputs.size() == 1);
    CHECK(thousand.outputs[0].bytes == expectedTarget(kCountedMaxCount, kCountedMaxCount));
}

//======================================================================================================================
TEST_CASE("C10m1's native arm draws its one point with one native call per counted draw", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult hundred = runConfiguration("C10m1-k100", Arm::Native, options);
    const ArmResult thousand = runConfiguration("C10m1-k1000", Arm::Native, options);
    CHECK(callsPerDraw(thousand, hundred) == 1.0);
    for (const std::string_view id : { "C10m1-k1", "C10m1-k100" })
    {
        CAPTURE(id);
        const ArmResult result = runConfiguration(id, Arm::Native, options);
        REQUIRE(result.outputs.size() == 1);
        CHECK(result.outputs[0].bytes == expectedSinglePoint());
    }
}

//======================================================================================================================
TEST_CASE("the native counted-draw helper encodes UInt16 indexed draws at maxCount 256 and 10,000", "[native]")
{
    const WorkloadEntry* entry = find("C10-k1");
    REQUIRE(entry != nullptr);
    for (const uint32_t maxCount : { 256u, 10'000u })
    {
        CAPTURE(maxCount);
        Config config = entry->config;
        config.k = 3;
        config.maxCount = maxCount;
        config.pattern = "indexed";
        const ArmResult result = runConfig(*entry, config, Arm::Native, runOptions(Mode::Correctness));
        REQUIRE(result.outputs.size() == 1);
        CHECK(result.outputs[0].bytes == expectedTarget(maxCount, maxCount));
    }
}
#endif
