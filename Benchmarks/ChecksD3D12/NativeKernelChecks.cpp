//----------------------------------------------------------------------------------------------------------------------
/// @file NativeKernelChecks.cpp
/// @brief Checks the native arms of the GPU kernel workloads on D3D12 against the CPU oracles of every thread.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/KernelPatterns.h"
#include "Common/Registry.h"
#include "Common/RunOptions.h"
#include "NativeD3D12/NativeContext.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
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

//======================================================================================================================
TEST_CASE("G1's native arm sums the 16 loads of every thread exactly", "[native]")
{
    const RunOptions options = runOptions(Mode::Correctness);
    for (const std::string_view id : { "G1-uniform", "G1-divergent" })
    {
        CAPTURE(id);
        const ArmResult result = runConfiguration(id, Arm::Native, options);
        REQUIRE(result.outputs.size() == 1);
        CHECK(result.outputs[0].name == "output");
        CHECK(result.outputs[0].bytes == expectedOutput(id));
        CHECK(result.nativeCalls == expectedFrameCalls(result, options));
    }
}

//======================================================================================================================
TEST_CASE("G2's native arms sum the 16 point samples of every thread exactly", "[native]")
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
        CHECK(table.outputs[0].name == "output");
        CHECK(fixed.outputs[0].name == "output");
        CHECK(table.outputs[0].bytes == expected);
        CHECK(fixed.outputs[0].bytes == expected);
    }
}
