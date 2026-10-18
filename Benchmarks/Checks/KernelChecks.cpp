//----------------------------------------------------------------------------------------------------------------------
/// @file KernelChecks.cpp
/// @brief Checks the GPU kernel workloads: RojoRHI's counts and calls on Null and the native arms on this device.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/CounterFields.h"
#include "Common/KernelPatterns.h"
#include "Common/Registry.h"
#include "Common/RunOptions.h"

#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI::bench;
using rojoRHI::Counters;

namespace {
    //==================================================================================================================
    ArmResult runConfiguration(std::string_view id, Arm arm, const RunOptions& options)
    {
        const WorkloadEntry* entry = find(id);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->runner(arm) != nullptr);
        ArmResult result = entry->runner(arm)(entry->config, options);
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(result.nativeCalls.size() == options.measuredFrames);
        return result;
    }

    //==================================================================================================================
    // What every arm must produce for a configuration: one "output" of 2^20 entries, G1's uint4 sums or G2's float sums.
    std::vector<std::byte> expectedOutput(std::string_view id)
    {
        const bool divergent = id.ends_with("divergent");
        std::vector<std::byte> bytes;
        if (id.starts_with("G1"))
        {
            bytes.resize(size_t{ kKernelThreads } * sizeof(DataEntry));
            for (uint32_t thread = 0; thread < kKernelThreads; ++thread)
            {
                const DataEntry sum = expectedLoads(thread, divergent);
                std::memcpy(bytes.data() + size_t{ thread } * sizeof(DataEntry), sum.data(), sizeof(DataEntry));
            }
        }
        else
        {
            bytes.resize(size_t{ kKernelThreads } * sizeof(float));
            for (uint32_t thread = 0; thread < kKernelThreads; ++thread)
            {
                const float sum = expectedSamples(thread, divergent);
                std::memcpy(bytes.data() + size_t{ thread } * sizeof(float), &sum, sizeof(float));
            }
        }
        return bytes;
    }
} // namespace

#if defined(ROJORHI_BENCH_BACKEND_NULL)
namespace {
    using rojoRHI::MessageSeverity;

    constexpr std::array<std::string_view, 4> kConfigurations{ "G1-uniform", "G1-divergent", "G2-uniform", "G2-divergent" };

    //==================================================================================================================
    // Three warm-up frames open each of the three frame slots once, so the measured frame reuses a slot whose list pool
    // and arena pages already exist.
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
    // One measured frame of G1 or G2 on Null, from the counter table of the Null backend's edges page: setPipeline,
    // setRoot (the second half of pushRoot) and dispatch each name a native call, so nativeRecordCalls is 3;
    // uploadFrameData names none but counts one frameDataUploads and the root's unpadded bytes; the one Queue::submit
    // without fences is one nativeSubmitCalls. G1's root is 24 bytes (two Addr, two uint32), G2's 32 (one Addr, six uint32).
    Counters kernelFrame(uint64_t rootBytes)
    {
        return Counters{ .nativeRecordCalls = 3, .nativeSubmitCalls = 1, .frameDataUploads = 1, .frameArenaBytes = rootBytes };
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
} // namespace

//======================================================================================================================
TEST_CASE("G1 and G2 through RojoRHI change every Null counter of a measured frame by its documented count", "[null]")
{
    for (const std::string_view id : kConfigurations)
    {
        CAPTURE(id);
        const ArmResult result = runConfiguration(id, Arm::Rojo, nullOptions(false, false));
        REQUIRE(result.frameCounters.has_value());
        checkEveryCounter(*result.frameCounters, kernelFrame(id.starts_with("G1") ? 24 : 32));
        CHECK(result.nativeCalls == std::vector<uint64_t>(2, 3 + 1));
        CHECK(result.nativeBarriers == std::vector<uint64_t>(2, 0));
        CHECK(*result.cpuAllocations == std::vector<uint64_t>(2, 0));
    }
}

//======================================================================================================================
TEST_CASE("one measured frame of G1 and G2 through RojoRHI makes exactly the registered calls in order", "[null]")
{
    const std::vector<std::string_view> frame{ "beginFrame", "acquireCommandList", "setPipeline", "uploadFrameData", "setRoot", "dispatch", "end", "submit",
                                               "endFrame" };
    for (const std::string_view id : kConfigurations)
    {
        CAPTURE(id);
        const ArmResult result = runConfiguration(id, Arm::Rojo, nullOptions(false, true));
        CHECK(result.trace == frame);
    }
}

//======================================================================================================================
TEST_CASE("G1 and G2 through RojoRHI run on a validating Null device without a message", "[null]")
{
#if defined(NDEBUG)
    SKIP("DeviceDesc::validation exists in Debug builds only");
#endif
    MessageLog log;
    rojoRHI::setMessageCallback(collectMessage, &log);
    for (const std::string_view id : kConfigurations)
    {
        CAPTURE(id);
        const ArmResult result = runConfiguration(id, Arm::Rojo, nullOptions(true, false));
        checkEveryCounter(*result.frameCounters, kernelFrame(id.starts_with("G1") ? 24 : 32));
    }
    rojoRHI::setMessageCallback(nullptr, nullptr);
    CHECK(log.lines.empty());
}
#endif

//======================================================================================================================
TEST_CASE("G1's native arm sums the 16 loads of every thread exactly under the validation layer", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    for (const std::string_view id : { "G1-uniform", "G1-divergent" })
    {
        CAPTURE(id);
        const ArmResult result = runConfiguration(id, Arm::Native, options);
        REQUIRE(result.outputs.size() == 1);
        CHECK(result.outputs[0].name == "output");
        CHECK(result.outputs[0].bytes == expectedOutput(id));
        CHECK(result.nativeCalls == std::vector<uint64_t>(options.measuredFrames, result.nativeCalls[0]));
        CHECK(result.gpuNs[0].has_value());
    }
}

//======================================================================================================================
TEST_CASE("G2's native arms sum the 16 point samples of every thread exactly under the validation layer", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    for (const std::string_view id : { "G2-uniform", "G2-divergent" })
    {
        CAPTURE(id);
        const std::vector<std::byte> expected = expectedOutput(id);
        const ArmResult table = runConfiguration(id, Arm::Native, options);
        const ArmResult fixed = runConfiguration(id, Arm::Fixed, options);
        REQUIRE(table.outputs.size() == 1);
        REQUIRE(fixed.outputs.size() == 1);
        CHECK(table.outputs[0].bytes == expected);
        CHECK(fixed.outputs[0].bytes == expected);
        CHECK(fixed.outputs[0].bytes == table.outputs[0].bytes);
        CHECK(table.gpuNs[0].has_value());
        CHECK(fixed.gpuNs[0].has_value());
    }
}
