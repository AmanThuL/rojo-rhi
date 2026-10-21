//----------------------------------------------------------------------------------------------------------------------
/// @file NativeCpuChecks.cpp
/// @brief Checks the native arms of the CPU operation workloads on this adapter: their calls and outputs against CPU oracles.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/CpuData.h"
#include "Common/CpuRoots.h"
#include "Common/Registry.h"
#include "Common/RunOptions.h"
#include "NativeD3D12/NativeContext.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rojoRHI::bench;

namespace {
    //==================================================================================================================
    // The native calls every measured frame should count: the last frame's count, less one for each frame that is the
    // first use of its slot, because the wait on a slot's zero fence value is skipped (frame index below the slot count).
    std::vector<uint64_t> expectedFrameCalls(const ArmResult& result, const RunOptions& options)
    {
        REQUIRE(options.warmupFrames + options.measuredFrames > NativeContext::kFrameSlots);
        const uint64_t steady = result.nativeCalls.back();
        std::vector<uint64_t> expected(options.measuredFrames, steady);
        for (uint32_t i = 0; i < options.measuredFrames; ++i)
        {
            if (options.warmupFrames + i < NativeContext::kFrameSlots)
            {
                expected[i] = steady - 1;
            }
        }
        return expected;
    }

    //==================================================================================================================
    // Runs the native arm of a configuration and fails the running case with the arm's error when it fails.
    ArmResult runNative(std::string_view id, const RunOptions& options)
    {
        const WorkloadEntry* entry = find(id);
        REQUIRE(entry != nullptr);
        REQUIRE(entry->runner(Arm::Native) != nullptr);
        ArmResult result = entry->runner(Arm::Native)(entry->config, options);
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(result.nativeCalls.size() == options.measuredFrames);
        return result;
    }

    //==================================================================================================================
    // Native calls per repetition by metric 2: the first measured frame's count at the high K less the low K, over their
    // difference.
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
    // C1's and C2's target after K draws: draw i colors pixel i mod 65,536, a later draw over an earlier one, and the rest
    // keeps the clear value of zero.
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

//======================================================================================================================
TEST_CASE("C1's native arm colors each repetition's pixel with two calls per repetition", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runNative("C1-k1", options);
    const ArmResult thousand = runNative("C1-k1000", options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 2.0);
    CHECK(outputBytes(one, "target") == drawnTarget(1));
    CHECK(outputBytes(thousand, "target") == drawnTarget(1000));
}

//======================================================================================================================
TEST_CASE("C1's native arm reads back 256×256×4 tight bytes", "[native]")
{
    const ArmResult result = runNative("C1-k10", runOptions(Mode::Correctness));
    REQUIRE(result.outputs.size() == 1);
    CHECK(result.outputs[0].name == "target");
    CHECK(result.outputs[0].bytes.size() == 256u * 256u * 4u);
    CHECK(result.outputs[0].bytes == drawnTarget(10));
}

//======================================================================================================================
TEST_CASE("C2's native arm colors each repetition's pixel through the index buffer with two calls per repetition", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runNative("C2-k1", options);
    const ArmResult thousand = runNative("C2-k1000", options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 2.0);
    CHECK(outputBytes(one, "target") == drawnTarget(1));
    CHECK(outputBytes(thousand, "target") == drawnTarget(1000));
}

//======================================================================================================================
TEST_CASE("C3's native arm alternates two pipelines with two calls per repetition and leaves B's depth", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runNative("C3-k1", options);
    const ArmResult thousand = runNative("C3-k1000", options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 2.0);
    for (const auto& [result, k] : { std::pair{ &one, 1u }, std::pair{ &thousand, 1000u } })
    {
        CAPTURE(k);
        CHECK(outputBytes(*result, "target") == plainTarget());
        CHECK(outputBytes(*result, "depth") == plainDepth(k));
    }
}

//======================================================================================================================
TEST_CASE("C4's native arm stores each repetition's value with two calls per repetition", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runNative("C4-k1", options);
    const ArmResult thousand = runNative("C4-k1000", options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 2.0);
    CHECK(outputBytes(one, "values") == storedValues(1));
    CHECK(outputBytes(thousand, "values") == storedValues(1000));
}

//======================================================================================================================
TEST_CASE("C5's native arm copies frame data with no native call per repetition", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult hundred = runNative("C5-k100", options);
    const ArmResult thousand = runNative("C5-k1000", options);
    CHECK(callsPerRepetition(thousand, 1000, hundred, 100) == 0.0);
    CHECK(thousand.outputs.empty());
    for (const ArmResult* result : { &hundred, &thousand })
    {
        CHECK(result->nativeCalls == expectedFrameCalls(*result, options));
        CHECK(result->nativeBarriers == std::vector<uint64_t>(options.measuredFrames, 0));
        CHECK(result->cpuNs[0].has_value());
    }
}

//======================================================================================================================
TEST_CASE("C6's native arm counts K dispatches separated by barriers with two calls per repetition", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runNative("C6-k1", options);
    const ArmResult thousand = runNative("C6-k1000", options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 2.0);
    CHECK(thousand.nativeBarriers[0] - one.nativeBarriers[0] == 999);
    CHECK(outputBytes(one, "counter") == counterBytes(1));
    CHECK(outputBytes(thousand, "counter") == counterBytes(1000));
}

//======================================================================================================================
TEST_CASE("C7's native arm copies the source pattern into every repetition's bytes with one call each", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult hundred = runNative("C7-k100", options);
    const ArmResult thousand = runNative("C7-k1000", options);
    CHECK(callsPerRepetition(thousand, 1000, hundred, 100) == 1.0);
    for (const auto& [result, k] : { std::pair{ &hundred, 100u }, std::pair{ &thousand, 1000u } })
    {
        CAPTURE(k);
        REQUIRE(result->outputs.size() == 1);
        CHECK(result->outputs[0].name == "dst");
        CHECK(result->outputs[0].bytes == copiedPattern(k));
    }
}

//======================================================================================================================
TEST_CASE("C8's native arm makes eight calls per pass, a barrier opening every pass but the first", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult one = runNative("C8-k1", options);
    const ArmResult thousand = runNative("C8-k1000", options);
    CHECK(callsPerRepetition(thousand, 1000, one, 1) == 8.0);
    CHECK(one.nativeBarriers[0] == 0);
    CHECK(thousand.nativeBarriers[0] == 999);
    CHECK(outputBytes(one, "target") == plainTarget());
    CHECK(outputBytes(thousand, "target") == plainTarget());
}

//======================================================================================================================
TEST_CASE("C8's native arm at K 10,000 resolves 20,000 timestamps", "[native]")
{
    // The arm's readback fails unless all 2K resolved timestamps of the last frame are nonzero and in recording order.
    const ArmResult result = runNative("C8-k10000", runOptions(Mode::Correctness));
    CHECK(outputBytes(result, "target") == plainTarget());
    CHECK(result.nativeBarriers[0] == 9999);
}

//======================================================================================================================
TEST_CASE("C9's native arm executes and signals with two calls per submission and copies every list's block", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    const ArmResult ten = runNative("C9-k10", options);
    const ArmResult hundred = runNative("C9-k100", options);
    CHECK(callsPerRepetition(hundred, 100, ten, 10) == 2.0);
    CHECK(outputBytes(ten, "dst") == copiedPattern(10));
    CHECK(outputBytes(hundred, "dst") == copiedPattern(100));
}
