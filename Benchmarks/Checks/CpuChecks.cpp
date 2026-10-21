//----------------------------------------------------------------------------------------------------------------------
/// @file CpuChecks.cpp
/// @brief Checks the CPU operation workloads: RojoRHI's counts and calls on Null and the native arm on this device.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/CounterFields.h"
#include "Common/CpuData.h"
#include "Common/CpuRoots.h"
#include "Common/Registry.h"
#include "Common/RunOptions.h"
#include "Native/NativeContext.h"

#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
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

    //==================================================================================================================
    // The same difference between any two K.
    double callsPerRepetition(const ArmResult& high, uint32_t kHigh, const ArmResult& low, uint32_t kLow)
    {
        return static_cast<double>(high.nativeCalls[0] - low.nativeCalls[0]) / static_cast<double>(kHigh - kLow);
    }

    //==================================================================================================================
    const std::vector<std::byte>& outputBytes(const ArmResult& result, std::string_view name)
    {
        for (const Output& output : result.outputs)
        {
            if (output.name == name)
            {
                return output.bytes;
            }
        }
        FAIL_CHECK("no output named " << name);
        return result.outputs.front().bytes;
    }

    //==================================================================================================================
    // C1's and C2's target after K draws: draw i colors pixel i mod 65,536, a later draw over an earlier one, and the
    // rest keeps the clear value of zero.
    std::vector<std::byte> drawnTarget(uint32_t k)
    {
        std::vector<std::byte> bytes(kTargetBytes, std::byte{ 0 });
        for (uint32_t index = 0; index < k; ++index)
        {
            const std::array<uint8_t, 4> color = drawColor(index);
            std::memcpy(bytes.data() + uint64_t{ index % kTargetPixels } * 4, color.data(), 4);
        }
        return bytes;
    }

    //==================================================================================================================
    // C3's and C8's target: BenchPlain.slang's triangle in its color over the cleared zero.
    std::vector<std::byte> plainTarget()
    {
        std::vector<std::byte> bytes(kTargetBytes, std::byte{ 0 });
        for (uint32_t y = 0; y < kTargetSize; ++y)
        {
            for (uint32_t x = 0; x < kTargetSize; ++x)
            {
                if (insidePlainTriangle(x, y))
                {
                    std::memcpy(bytes.data() + (uint64_t{ y } * kTargetSize + x) * 4, kPlainColor.data(), 4);
                }
            }
        }
        return bytes;
    }

    //==================================================================================================================
    // C3's depth, cleared to 0: pipeline A, used by even repetitions, writes no depth, and pipeline B, used by odd ones,
    // writes the triangle's 0.5 where it passes Greater, so K of 2 or more leaves 0.5 inside the triangle.
    std::vector<std::byte> plainDepth(uint32_t k)
    {
        std::vector<std::byte> bytes(kTargetBytes, std::byte{ 0 });
        if (k < 2)
        {
            return bytes;
        }
        for (uint32_t y = 0; y < kTargetSize; ++y)
        {
            for (uint32_t x = 0; x < kTargetSize; ++x)
            {
                if (insidePlainTriangle(x, y))
                {
                    std::memcpy(bytes.data() + (uint64_t{ y } * kTargetSize + x) * 4, &kPlainDepth, 4);
                }
            }
        }
        return bytes;
    }

    //==================================================================================================================
    // C4's output: dispatch i stores its value at element i.
    std::vector<std::byte> storedValues(uint32_t k)
    {
        std::vector<std::byte> bytes(uint64_t{ k } * 4);
        for (uint32_t index = 0; index < k; ++index)
        {
            const uint32_t value = storeValue(index);
            std::memcpy(bytes.data() + uint64_t{ index } * 4, &value, 4);
        }
        return bytes;
    }

    //==================================================================================================================
    // C6's output: the counter after K dispatches that each add one to the zero the frame filled.
    std::vector<std::byte> counterBytes(uint32_t k)
    {
        std::vector<std::byte> bytes(4);
        std::memcpy(bytes.data(), &k, 4);
        return bytes;
    }

    //==================================================================================================================
    // C7's and C9's output: K back-to-back copies of the fixed pattern, built on the CPU from the pattern alone.
    std::vector<std::byte> copiedPattern(uint32_t k)
    {
        std::vector<std::byte> expected;
        const std::array<std::byte, kCopyBytes> pattern = copyPattern();
        for (uint32_t repetition = 0; repetition < k; ++repetition)
        {
            expected.insert(expected.end(), pattern.begin(), pattern.end());
        }
        return expected;
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
    // A workload at a K the sweep does not register, through the runner of its lowest K's configuration.
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

    //==================================================================================================================
    // C1 and C2 on Null: beginRenderPass, setPipeline, setViewport, setScissor and endRenderPass are five native calls
    // and each repetition's setRoot and draw (or drawIndexed) two more, while its uploadFrameData of the 64-byte root
    // names none and adds one upload of 64 unpadded bytes; the one pass is the frame's one encodersOpened; one submit.
    Counters c1Frame(uint64_t k)
    {
        return Counters{ .nativeRecordCalls = 5 + 2 * k, .nativeSubmitCalls = 1, .frameDataUploads = k, .frameArenaBytes = 64 * k, .encodersOpened = 1 };
    }

    //==================================================================================================================
    // C3 on Null: beginRenderPass, setViewport, setScissor and endRenderPass are four native calls and each
    // repetition's setPipeline and draw two more; no frame data; one pass, one submit.
    Counters c3Frame(uint64_t k)
    {
        return Counters{ .nativeRecordCalls = 4 + 2 * k, .nativeSubmitCalls = 1, .encodersOpened = 1 };
    }

    //==================================================================================================================
    // C4 on Null: setPipeline is one native call and each repetition's setRoot and dispatch two more, while its
    // uploadFrameData of the 16-byte root adds one upload of 16 bytes; no pass; one submit.
    Counters c4Frame(uint64_t k)
    {
        return Counters{ .nativeRecordCalls = 1 + 2 * k, .nativeSubmitCalls = 1, .frameDataUploads = k, .frameArenaBytes = 16 * k };
    }

    //==================================================================================================================
    // C6 on Null: the counter's fillBuffer, the barrier ordering it before the dispatches, setPipeline and the setRoot
    // of the 8-byte root are four native calls and one upload; each repetition's dispatch and barrier two more. Every
    // barrier call holds one global record and is one nativeBarriers. One submit.
    Counters c6Frame(uint64_t k)
    {
        return Counters{ .nativeRecordCalls = 4 + 2 * k,
                         .nativeSubmitCalls = 1,
                         .frameDataUploads = 1,
                         .frameArenaBytes = 8,
                         .globalBarriers = k + 1,
                         .nativeBarriers = k + 1 };
    }

    //==================================================================================================================
    // C8 on Null: each repetition's beginRenderPass, setPipeline, setViewport, setScissor, draw and endRenderPass are
    // six native calls and one encodersOpened, and every repetition but the first adds a barrier call with one texture
    // record, one more native call and one nativeBarriers; the pass scope's label adds no frame data; one submit.
    Counters c8Frame(uint64_t k)
    {
        return Counters{ .nativeRecordCalls = 7 * k - 1, .nativeSubmitCalls = 1, .textureBarriers = k - 1, .nativeBarriers = k - 1, .encodersOpened = k };
    }

    //==================================================================================================================
    // C9 on Null: K lists of one copyBuffer each are K native record calls, and each of the K submits is one
    // nativeSubmitCalls plus one for the fence value it signals.
    Counters c9Frame(uint64_t k)
    {
        return Counters{ .nativeRecordCalls = k, .nativeSubmitCalls = 2 * k };
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

    //==================================================================================================================
    // One frame of one list: `before` once, `repetition` K times, then `after`, between acquisition and end.
    std::vector<std::string_view> listCalls(
        std::initializer_list<std::string_view> before,
        std::initializer_list<std::string_view> repetition,
        std::initializer_list<std::string_view> after,
        uint32_t k
    )
    {
        std::vector<std::string_view> calls{ "beginFrame", "acquireCommandList" };
        calls.insert(calls.end(), before);
        for (uint32_t index = 0; index < k; ++index)
        {
            calls.insert(calls.end(), repetition);
        }
        calls.insert(calls.end(), after);
        calls.insert(calls.end(), { "end", "submit", "endFrame" });
        return calls;
    }

    //==================================================================================================================
    // C8's frame: K passes in one list, a barrier before every pass but the first.
    std::vector<std::string_view> c8Calls(uint32_t k)
    {
        std::vector<std::string_view> calls{ "beginFrame", "acquireCommandList" };
        for (uint32_t index = 0; index < k; ++index)
        {
            if (index > 0)
            {
                calls.push_back("barrier");
            }
            calls.insert(calls.end(), { "beginRenderPass", "setPipeline", "setViewport", "setScissor", "draw", "endRenderPass" });
        }
        calls.insert(calls.end(), { "end", "submit", "endFrame" });
        return calls;
    }

    //==================================================================================================================
    // C9's frame: K lists recorded first, then K submissions.
    std::vector<std::string_view> c9Calls(uint32_t k)
    {
        std::vector<std::string_view> calls{ "beginFrame" };
        for (uint32_t index = 0; index < k; ++index)
        {
            calls.insert(calls.end(), { "acquireCommandList", "copyBuffer", "end" });
        }
        calls.insert(calls.end(), k, "submit");
        calls.push_back("endFrame");
        return calls;
    }

    // One workload's expected Null counts: the frame's counters, its native calls per frame (C9: of its submit calls)
    // and its calls per repetition, at metric 2's K pair.
    struct NullRow
    {
        std::string_view workload;
        uint32_t low = 100;
        uint32_t high = 1000;
        Counters (*frame)(uint64_t k) = nullptr;
        uint64_t fixedCalls = 0; ///< Native calls per frame besides the repetitions'.
        uint64_t callsPerRepetition = 0;
    };

    // C9's counted window holds the queue and host calls of its K submits: each submit and the signal it makes.
    constexpr std::array<NullRow, 7> kNullRows{
        NullRow{ .workload = "C1", .frame = c1Frame, .fixedCalls = 6, .callsPerRepetition = 2 },
        NullRow{ .workload = "C2", .frame = c1Frame, .fixedCalls = 6, .callsPerRepetition = 2 },
        NullRow{ .workload = "C3", .frame = c3Frame, .fixedCalls = 5, .callsPerRepetition = 2 },
        NullRow{ .workload = "C4", .frame = c4Frame, .fixedCalls = 2, .callsPerRepetition = 2 },
        NullRow{ .workload = "C6", .frame = c6Frame, .fixedCalls = 5, .callsPerRepetition = 2 },
        NullRow{ .workload = "C8", .frame = c8Frame, .fixedCalls = 0, .callsPerRepetition = 7 },
        NullRow{ .workload = "C9", .low = 10, .high = 100, .frame = c9Frame, .fixedCalls = 0, .callsPerRepetition = 2 },
    };

    //==================================================================================================================
    std::string idAt(std::string_view workload, uint32_t k)
    {
        return std::string(workload) + "-k" + std::to_string(k);
    }

    //==================================================================================================================
    // The calls one measured frame of a workload makes through RojoRHI, at K.
    std::vector<std::string_view> tracedFrame(std::string_view workload, uint32_t k)
    {
        if (workload == "C1")
        {
            return listCalls(
                { "beginRenderPass", "setPipeline", "setViewport", "setScissor" }, { "uploadFrameData", "setRoot", "draw" }, { "endRenderPass" }, k
            );
        }
        if (workload == "C2")
        {
            return listCalls(
                { "beginRenderPass", "setPipeline", "setViewport", "setScissor" }, { "uploadFrameData", "setRoot", "drawIndexed" }, { "endRenderPass" }, k
            );
        }
        if (workload == "C3")
        {
            return listCalls({ "beginRenderPass", "setViewport", "setScissor" }, { "setPipeline", "draw" }, { "endRenderPass" }, k);
        }
        if (workload == "C4")
        {
            return listCalls({ "setPipeline" }, { "uploadFrameData", "setRoot", "dispatch" }, {}, k);
        }
        if (workload == "C6")
        {
            return listCalls({ "fillBuffer", "barrier", "setPipeline", "uploadFrameData", "setRoot" }, { "dispatch", "barrier" }, {}, k);
        }
        if (workload == "C8")
        {
            return c8Calls(k);
        }
        return c9Calls(k);
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

//======================================================================================================================
TEST_CASE("C1 to C4, C6, C8 and C9 through RojoRHI change every Null counter of a measured frame by its documented count", "[null]")
{
    for (const NullRow& row : kNullRows)
    {
        for (const uint32_t k : { row.low, row.high })
        {
            CAPTURE(row.workload, k);
            const ArmResult result = runConfiguration(idAt(row.workload, k), Arm::Rojo, nullOptions(false, false));
            REQUIRE(result.frameCounters.has_value());
            checkEveryCounter(*result.frameCounters, row.frame(k));
            CHECK(result.nativeCalls == std::vector<uint64_t>(2, row.fixedCalls + row.callsPerRepetition * k));
            CHECK(result.nativeBarriers == std::vector<uint64_t>(2, row.frame(k).nativeBarriers));
            CHECK(*result.cpuAllocations == std::vector<uint64_t>(2, 0));
        }
    }
}

//======================================================================================================================
TEST_CASE("C1 to C4, C6, C8 and C9 through RojoRHI make their documented Null calls per repetition", "[null]")
{
    for (const NullRow& row : kNullRows)
    {
        CAPTURE(row.workload);
        const ArmResult low = runConfiguration(idAt(row.workload, row.low), Arm::Rojo, nullOptions(false, false));
        const ArmResult high = runConfiguration(idAt(row.workload, row.high), Arm::Rojo, nullOptions(false, false));
        CHECK(callsPerRepetition(high, row.high, low, row.low) == static_cast<double>(row.callsPerRepetition));
        const Counters perRepetitions = counterDelta(*high.frameCounters, *low.frameCounters);
        const Counters expected = counterDelta(row.frame(row.high), row.frame(row.low));
        checkEveryCounter(perRepetitions, expected);
    }
}

//======================================================================================================================
TEST_CASE("one measured frame of C1 to C4, C6, C8 and C9 through RojoRHI makes exactly the registered calls in order", "[null]")
{
    for (const NullRow& row : kNullRows)
    {
        for (const uint32_t k : { 1u, 2u })
        {
            CAPTURE(row.workload, k);
            const ArmResult result = runAtK(row.workload, k, Arm::Rojo, nullOptions(false, true));
            CHECK(result.trace == tracedFrame(row.workload, k));
        }
    }
}

//======================================================================================================================
TEST_CASE("C1 to C4, C6, C8 and C9 through RojoRHI run on a validating Null device without a message", "[null]")
{
#if defined(NDEBUG)
    SKIP("DeviceDesc::validation exists in Debug builds only");
#endif
    const MessageCapture messages;
    for (const NullRow& row : kNullRows)
    {
        for (const uint32_t k : { 1u, 2u, row.low, row.high })
        {
            CAPTURE(row.workload, k);
            const ArmResult result = runAtK(row.workload, k, Arm::Rojo, nullOptions(true, k <= 2));
            checkEveryCounter(*result.frameCounters, row.frame(k));
            // The traced sequence is the same with the tracker on.
            if (k <= 2)
            {
                CHECK(result.trace == tracedFrame(row.workload, k));
            }
        }
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
        CHECK(result->outputs[0].bytes == copiedPattern(k));
        CHECK(result->gpuNs[0].has_value());
    }
}

//======================================================================================================================
TEST_CASE("C1's native arm colors each repetition's pixel with two calls per repetition under the validation layer", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runConfiguration("C1-k1", Arm::Native, options);
    const ArmResult thousand = runConfiguration("C1-k1000", Arm::Native, options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 2.0);
    CHECK(outputBytes(one, "target") == drawnTarget(1));
    CHECK(outputBytes(thousand, "target") == drawnTarget(1000));
    CHECK(thousand.gpuNs[0].has_value());
}

//======================================================================================================================
TEST_CASE("C2's native arm colors each repetition's pixel through the index buffer with two calls per repetition", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runConfiguration("C2-k1", Arm::Native, options);
    const ArmResult thousand = runConfiguration("C2-k1000", Arm::Native, options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 2.0);
    CHECK(outputBytes(one, "target") == drawnTarget(1));
    CHECK(outputBytes(thousand, "target") == drawnTarget(1000));
}

//======================================================================================================================
TEST_CASE("C3's native arm alternates two pipelines with four calls per repetition and leaves B's depth", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runConfiguration("C3-k1", Arm::Native, options);
    const ArmResult thousand = runConfiguration("C3-k1000", Arm::Native, options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 4.0);
    for (const auto& [result, k] : { std::pair{ &one, 1u }, std::pair{ &thousand, 1000u } })
    {
        CAPTURE(k);
        CHECK(outputBytes(*result, "target") == plainTarget());
        CHECK(outputBytes(*result, "depth") == plainDepth(k));
    }
}

//======================================================================================================================
TEST_CASE("C4's native arm stores each repetition's value with two calls per repetition under the validation layer", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runConfiguration("C4-k1", Arm::Native, options);
    const ArmResult thousand = runConfiguration("C4-k1000", Arm::Native, options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 2.0);
    CHECK(outputBytes(one, "values") == storedValues(1));
    CHECK(outputBytes(thousand, "values") == storedValues(1000));
}

//======================================================================================================================
TEST_CASE("C6's native arm counts K dispatches separated by barriers with two calls per repetition", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runConfiguration("C6-k1", Arm::Native, options);
    const ArmResult thousand = runConfiguration("C6-k1000", Arm::Native, options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 2.0);
    CHECK(thousand.nativeBarriers[0] - one.nativeBarriers[0] == 999);
    CHECK(outputBytes(one, "counter") == counterBytes(1));
    CHECK(outputBytes(thousand, "counter") == counterBytes(1000));
}

//======================================================================================================================
TEST_CASE("C8's native arm makes twelve calls per pass, a barrier opening every pass but the first", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runConfiguration("C8-k1", Arm::Native, options);
    const ArmResult thousand = runConfiguration("C8-k1000", Arm::Native, options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 12.0);
    CHECK(one.nativeBarriers[0] == 0);
    CHECK(thousand.nativeBarriers[0] == 999);
    CHECK(outputBytes(one, "target") == plainTarget());
    CHECK(outputBytes(thousand, "target") == plainTarget());
}

//======================================================================================================================
TEST_CASE("C9's native arm commits and signals with two calls per submission and copies every list's block", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult ten = runConfiguration("C9-k10", Arm::Native, options);
    const ArmResult hundred = runConfiguration("C9-k100", Arm::Native, options);
    CHECK(callsPerRepetition(hundred, 100, ten, 10) == 2.0);
    CHECK(outputBytes(ten, "dst") == copiedPattern(10));
    CHECK(outputBytes(hundred, "dst") == copiedPattern(100));
    CHECK(hundred.gpuNs[0].has_value());
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

//======================================================================================================================
TEST_CASE("the native arm signals its frame event at each frame's end and waits on it before reusing a frame slot", "[native]")
{
    // Every frame makes the same two calls, its first use of a slot included: the wait for a value the event already holds
    // returns at once. Frame index signals index + 1, and no later signal is queued before the check, so a repeated or
    // skipped value fails it.
    NativeContext context;
    REQUIRE(context.open(runOptions(Mode::Correctness)).has_value());
    REQUIRE(context.prepareFrames(NativeFrameShape{}).has_value());
    REQUIRE(context.frameEvent() != nullptr);
    constexpr uint32_t kFrames = 2 * NativeContext::kFrameSlots;
    for (uint32_t index = 0; index < kFrames; ++index)
    {
        CAPTURE(index);
        const NativeCalls before = context.calls();
        (void)context.beginFrame(index, 0);
        CHECK(context.calls().host == before.host + 1);
        REQUIRE(context.endFrame().has_value());
        CHECK(context.calls().submit == before.submit + 1);
        CHECK(context.calls().host == before.host + 1);
        CHECK(context.calls().record == before.record);
        const uint64_t value = uint64_t{ index } + 1;
        REQUIRE(context.frameEvent()->waitUntilSignaledValue(value, 10'000));
        CHECK(context.frameEvent()->signaledValue() == value);
    }
}
