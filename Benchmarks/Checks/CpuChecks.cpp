//----------------------------------------------------------------------------------------------------------------------
/// @file CpuChecks.cpp
/// @brief Checks the CPU operation workloads: RojoRHI's counts and calls on Null and the native arm on this device.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/CounterFields.h"
#include "Common/CpuData.h"
#include "Common/Registry.h"
#include "Common/RunOptions.h"
#include "Native/NativeContext.h"

#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rojoRHI::bench;
using rojoRHI::Counters;

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
    // Native calls per repetition by metric 2: the first measured frame's count at K 1,000 less K 100, over 900.
    double callsPerRepetition(const ArmResult& atThousand, const ArmResult& atHundred)
    {
        return static_cast<double>(atThousand.nativeCalls[0] - atHundred.nativeCalls[0]) / 900.0;
    }
} // namespace

#if defined(ROJORHI_BENCH_BACKEND_NULL)
namespace {
    using rojoRHI::MessageSeverity;

    //==================================================================================================================
    // Three warm-up frames open each of the three frame slots once, so the first measured frame reuses a slot whose list
    // pool and arena pages already exist; correctness mode's two would leave the third slot's first use measured.
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
    // C5 on Null, from the counter table of the Null backend's edges page: uploadFrameData names no native call, so
    // nativeRecordCalls stays 0 and each upload adds one frameDataUploads and its 256 unpadded bytes; acquisition and
    // end count nothing; the one Queue::submit without fences is one nativeSubmitCalls; beginFrame, endFrame and the
    // reused list and arena pages of the slot create, allocate and grow nothing.
    Counters c5Frame(uint64_t k)
    {
        return Counters{ .nativeSubmitCalls = 1, .frameDataUploads = k, .frameArenaBytes = 256 * k };
    }

    //==================================================================================================================
    // C7 on Null: copyBuffer names one native call, so nativeRecordCalls is K; one submit without fences; Null counts
    // encodersOpened at beginRenderPass only, and C7 opens no pass.
    Counters c7Frame(uint64_t k)
    {
        return Counters{ .nativeRecordCalls = k, .nativeSubmitCalls = 1 };
    }

    // Collects every message RojoRHI emits while it lives, which the default sink would print, and restores that sink
    // however the case ends, so no callback outlives the log.
    class MessageCapture
    {
      public:
        MessageCapture();
        ~MessageCapture();

        std::vector<std::string> lines;
    };

    //==================================================================================================================
    void collectMessage(MessageSeverity /*severity*/, std::string_view message, void* user)
    {
        static_cast<MessageCapture*>(user)->lines.emplace_back(message);
    }

    //==================================================================================================================
    MessageCapture::MessageCapture()
    {
        rojoRHI::setMessageCallback(collectMessage, this);
    }

    //==================================================================================================================
    MessageCapture::~MessageCapture()
    {
        rojoRHI::setMessageCallback(nullptr, nullptr);
    }

    //==================================================================================================================
    std::vector<std::string_view> frameCalls(std::string_view operation, uint32_t k)
    {
        std::vector<std::string_view> calls{ "beginFrame", "acquireCommandList" };
        calls.insert(calls.end(), k, operation);
        calls.insert(calls.end(), { "end", "submit", "endFrame" });
        return calls;
    }
} // namespace

//======================================================================================================================
TEST_CASE("C5 through RojoRHI changes every Null counter of a measured frame by its documented count", "[null]")
{
    for (const uint64_t k : { 100u, 1000u })
    {
        CAPTURE(k);
        const ArmResult result = runConfiguration("C5-k" + std::to_string(k), Arm::Rojo, nullOptions(false, false));
        REQUIRE(result.frameCounters.has_value());
        checkEveryCounter(*result.frameCounters, c5Frame(k));
        CHECK(result.nativeCalls == std::vector<uint64_t>(2, 1));
        CHECK(result.nativeBarriers == std::vector<uint64_t>(2, 0));
        CHECK(*result.cpuAllocations == std::vector<uint64_t>(2, 0));
    }
}

//======================================================================================================================
TEST_CASE("C5 through RojoRHI adds one frame upload of 256 bytes and no native call per repetition on Null", "[null]")
{
    const ArmResult hundred = runConfiguration("C5-k100", Arm::Rojo, nullOptions(false, false));
    const ArmResult thousand = runConfiguration("C5-k1000", Arm::Rojo, nullOptions(false, false));
    CHECK(callsPerRepetition(thousand, hundred) == 0.0);
    const Counters perRepetition = counterDelta(*thousand.frameCounters, *hundred.frameCounters);
    CHECK(perRepetition.frameDataUploads == 900);
    CHECK(perRepetition.frameArenaBytes == 900 * 256);
}

//======================================================================================================================
TEST_CASE("C7 through RojoRHI changes every Null counter of a measured frame by its documented count", "[null]")
{
    for (const uint64_t k : { 100u, 1000u })
    {
        CAPTURE(k);
        const ArmResult result = runConfiguration("C7-k" + std::to_string(k), Arm::Rojo, nullOptions(false, false));
        REQUIRE(result.frameCounters.has_value());
        checkEveryCounter(*result.frameCounters, c7Frame(k));
        CHECK(result.nativeCalls == std::vector<uint64_t>(2, k + 1));
        CHECK(*result.cpuAllocations == std::vector<uint64_t>(2, 0));
    }
}

//======================================================================================================================
TEST_CASE("C7 through RojoRHI makes one native call per repetition on Null", "[null]")
{
    const ArmResult hundred = runConfiguration("C7-k100", Arm::Rojo, nullOptions(false, false));
    const ArmResult thousand = runConfiguration("C7-k1000", Arm::Rojo, nullOptions(false, false));
    CHECK(callsPerRepetition(thousand, hundred) == 1.0);
}

//======================================================================================================================
TEST_CASE("one measured frame of C5 and C7 through RojoRHI makes exactly the registered calls in order", "[null]")
{
    for (const uint32_t k : { 1u, 2u })
    {
        CAPTURE(k);
        const ArmResult c5 = runAtK("C5", k, Arm::Rojo, nullOptions(false, true));
        CHECK(c5.trace == frameCalls("uploadFrameData", k));
        const ArmResult c7 = runAtK("C7", k, Arm::Rojo, nullOptions(false, true));
        CHECK(c7.trace == frameCalls("copyBuffer", k));
    }
}

//======================================================================================================================
TEST_CASE("C5 and C7 through RojoRHI run on a validating Null device without a message", "[null]")
{
#if defined(NDEBUG)
    SKIP("DeviceDesc::validation exists in Debug builds only");
#endif
    const MessageCapture messages;
    for (const std::string_view id : { "C5-k100", "C5-k1000", "C7-k100", "C7-k1000" })
    {
        CAPTURE(id);
        const ArmResult result = runConfiguration(id, Arm::Rojo, nullOptions(true, false));
        const uint64_t k = std::stoull(std::string(id.substr(4)));
        checkEveryCounter(*result.frameCounters, id.starts_with("C5") ? c5Frame(k) : c7Frame(k));
    }
    // The traced sequence is the same with the tracker on.
    for (const uint32_t k : { 1u, 2u })
    {
        CAPTURE(k);
        CHECK(runAtK("C5", k, Arm::Rojo, nullOptions(true, true)).trace == frameCalls("uploadFrameData", k));
        CHECK(runAtK("C7", k, Arm::Rojo, nullOptions(true, true)).trace == frameCalls("copyBuffer", k));
    }
    CHECK(messages.lines.empty());
}
#endif

//======================================================================================================================
TEST_CASE("C5's native arm copies frame data with no native call per repetition under the validation layer", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult hundred = runConfiguration("C5-k100", Arm::Native, options);
    const ArmResult thousand = runConfiguration("C5-k1000", Arm::Native, options);
    CHECK(callsPerRepetition(thousand, hundred) == 0.0);
    CHECK(thousand.outputs.empty());
    for (const ArmResult* result : { &hundred, &thousand })
    {
        CHECK(result->nativeCalls == std::vector<uint64_t>(options.measuredFrames, result->nativeCalls[0]));
        CHECK(result->nativeBarriers == std::vector<uint64_t>(options.measuredFrames, 0));
        CHECK(result->cpuNs[0].has_value());
        CHECK(result->gpuNs[0].has_value());
    }
}

//======================================================================================================================
TEST_CASE("C7's native arm copies the source pattern into every repetition's bytes with one call each", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult hundred = runConfiguration("C7-k100", Arm::Native, options);
    const ArmResult thousand = runConfiguration("C7-k1000", Arm::Native, options);
    CHECK(callsPerRepetition(thousand, hundred) == 1.0);
    for (const auto& [result, k] : { std::pair{ &hundred, 100u }, std::pair{ &thousand, 1000u } })
    {
        CAPTURE(k);
        REQUIRE(result->outputs.size() == 1);
        CHECK(result->outputs[0].name == "dst");
        // The oracle: K back-to-back copies of the fixed pattern, built on the CPU from the pattern alone.
        std::vector<std::byte> expected;
        const std::array<std::byte, kCopyBytes> pattern = copyPattern();
        for (uint32_t repetition = 0; repetition < k; ++repetition)
        {
            expected.insert(expected.end(), pattern.begin(), pattern.end());
        }
        CHECK(result->outputs[0].bytes == expected);
        CHECK(result->gpuNs[0].has_value());
    }
}

//======================================================================================================================
TEST_CASE("a native shader library that was not built is reported with its path", "[native]")
{
    NativeContext context;
    REQUIRE(context.open(runOptions(Mode::Correctness)).has_value());
    const auto library = context.library("BenchMissing");
    REQUIRE(!library.has_value());
    CHECK(library.error().find("Shaders/BenchMissing.metallib") != std::string::npos);
}
