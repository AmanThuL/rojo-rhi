//----------------------------------------------------------------------------------------------------------------------
/// @file HarnessChecks.cpp
/// @brief Checks the harness pieces both arms share: the run JSON, the configuration list, commit feedback and outputs.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/FeedbackCollector.h"
#include "Common/Outputs.h"
#include "Common/Registry.h"
#include "Common/Results.h"
#include "Common/RunOptions.h"
#include "Rojo/RojoGpuTiming.h"

#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace rojoRHI::bench;

namespace {
#if defined(NDEBUG)
    constexpr std::string_view kBuild = "release";
#else
    constexpr std::string_view kBuild = "debug";
#endif
#if defined(ROJORHI_BENCH_BACKEND_METAL4)
    constexpr std::string_view kBackend = "metal4";
#else
    constexpr std::string_view kBackend = "null";
#endif

    // SHA-256 of the three bytes "abc", from FIPS 180-2 appendix B.1.
    constexpr std::string_view kAbcSha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    //==================================================================================================================
    std::vector<std::byte> bytesOf(std::string_view text)
    {
        std::vector<std::byte> bytes;
        for (const char c : text)
        {
            bytes.push_back(static_cast<std::byte>(c));
        }
        return bytes;
    }

    //==================================================================================================================
    // The contract's twenty counter fields, spelled out here so a renamed or dropped field fails the comparison.
    std::string countersText(uint64_t recordCalls)
    {
        std::ostringstream text;
        text << R"({"nativeCreateCalls":0,"nativeRecordCalls":)" << recordCalls
             << R"(,"nativeSubmitCalls":0,"nativeHostCalls":0,"cpuAllocations":0,"gpuAllocations":0,"frameDataUploads":0,)"
             << R"("frameArenaBytes":0,"frameArenaGrowths":0,"descriptorWrites":0,"globalBarriers":0,"bufferBarriers":0,)"
             << R"("textureBarriers":0,"nativeBarriers":0,"pipelinesCreated":0,"pipelineCacheHits":0,"pipelineCacheMisses":0,)"
             << R"("pipelineCompileNanoseconds":0,"encodersOpened":0,"deferredDestructionsRetired":0})";
        return text.str();
    }

    //==================================================================================================================
    Config syntheticConfig()
    {
        return Config{ .id = "C7-k100", .workload = "C7", .k = 100, .arms = { Arm::Rojo, Arm::Native } };
    }

    //==================================================================================================================
    ArmResult syntheticRojo()
    {
        ArmResult r;
        r.arm = Arm::Rojo;
        r.cpuNs = { 1.5 };
        r.gpuNs = { std::nullopt };
        r.allocations = { 2 };
        r.nativeCalls = { 101 };
        r.nativeBarriers = { 0 };
        r.cpuAllocations = std::vector<uint64_t>{ 0 };
        r.countersAfterWarmup = rojoRHI::Counters{ .nativeRecordCalls = 5 };
        r.countersEnd = rojoRHI::Counters{ .nativeRecordCalls = 7 };
        r.frameCounters = rojoRHI::Counters{ .nativeRecordCalls = 1 };
        r.outputs = { Output{ "dst", bytesOf("abc") } };
        return r;
    }

    //==================================================================================================================
    ArmResult syntheticNative()
    {
        ArmResult n;
        n.arm = Arm::Native;
        n.cpuNs = { 1.25 };
        n.gpuNs = { 2000.0 };
        n.allocations = { 3 };
        n.nativeCalls = { 104 };
        n.nativeBarriers = { 0 };
        n.outputs = { Output{ "dst", bytesOf("abc") } };
        return n;
    }
} // namespace

//======================================================================================================================
TEST_CASE("the run JSON of two arms carries every field of the harness contract", "[host]")
{
    const Config config = syntheticConfig();
    const std::array<ArmResult, 2> arms{ syntheticRojo(), syntheticNative() };
    const RunRecord record{
        .config = config,
        .mode = Mode::Correctness,
        .validation = false,
        .device = "Test GPU",
        .os = "Test OS",
        .thermalStart = "nominal",
        .thermalEnd = "fair",
        .arms = arms,
    };
    const std::string output = R"({"name":"dst","bytes":3,"sha256":")" + std::string(kAbcSha256) + R"("})";
    const std::string expected =
        R"({"schema":1,"id":"C7-k100","workload":"C7","params":{"K":100},"mode":"correctness","build":")" + std::string(kBuild) + R"(","backend":")" +
        std::string(kBackend) + R"(","validation":false,"order":["R","N"],"device":"Test GPU","os":"Test OS","thermalStart":"nominal",)" +
        R"("thermalEnd":"fair","arms":{"R":{"status":"ok","error":"","cpuNs":[1.5],"gpuNs":[null],"allocations":[2],)" +
        R"("nativeCalls":[101],"nativeBarriers":[0],"cpuAllocations":[0],"counters":{"afterWarmup":)" + countersText(5) + R"(,"end":)" + countersText(7) +
        R"(},"frameCounters":)" + countersText(1) + R"(,"outputs":[)" + output +
        R"(]},"N":{"status":"ok","error":"","cpuNs":[1.25],"gpuNs":[2000],"allocations":[3],"nativeCalls":[104],)" +
        R"("nativeBarriers":[0],"cpuAllocations":null,"counters":null,"frameCounters":null,"outputs":[)" + output + R"(]}},"outputsEqual":true})";
    CHECK(runJson(record) == expected);
}

//======================================================================================================================
TEST_CASE("a run of the native arm alone leaves the RojoRHI arm out and the comparison null", "[host]")
{
    const Config config = syntheticConfig();
    ArmResult native = syntheticNative();
    native.ok = false;
    native.error = "C7-k100: commit failed";
    const std::array<ArmResult, 1> arms{ native };
    const RunRecord record{
        .config = config, .mode = Mode::Timing, .validation = true, .device = "d", .os = "o", .thermalStart = "nominal", .thermalEnd = "nominal", .arms = arms
    };
    const std::string text = runJson(record);
    CHECK(text.find(R"("order":["N"])") != std::string::npos);
    CHECK(text.find(R"("arms":{"N":{"status":"failed","error":"C7-k100: commit failed",)") != std::string::npos);
    CHECK(text.find(R"("R":)") == std::string::npos);
    CHECK(text.find(R"("mode":"timing")") != std::string::npos);
    CHECK(text.find(R"("validation":true)") != std::string::npos);
    CHECK(text.ends_with(R"(,"outputsEqual":null})"));
}

//======================================================================================================================
TEST_CASE("outputs compare equal only when every name and byte matches", "[host]")
{
    std::array<ArmResult, 2> arms{ syntheticRojo(), syntheticNative() };
    CHECK(outputsEqual(arms, Arm::Rojo, Arm::Native) == std::optional<bool>(true));
    arms[1].outputs[0].bytes[2] = std::byte{ 0 };
    CHECK(outputsEqual(arms, Arm::Rojo, Arm::Native) == std::optional<bool>(false));
    arms[1].outputs[0] = Output{ "other", bytesOf("abc") };
    CHECK(outputsEqual(arms, Arm::Rojo, Arm::Native) == std::optional<bool>(false));
    arms[1].ok = false;
    CHECK(outputsEqual(arms, Arm::Rojo, Arm::Native) == std::nullopt);
    CHECK(outputsEqual(std::span(arms).first(1), Arm::Rojo, Arm::Native) == std::nullopt);
}

//======================================================================================================================
TEST_CASE("the configuration list prints one object per registered configuration with a unique id", "[host]")
{
    const std::vector<WorkloadEntry>& entries = registry();
    REQUIRE(!entries.empty());
    std::set<std::string> ids;
    for (const WorkloadEntry& entry : entries)
    {
        const std::string line = listJson(entry.config);
        CHECK(line.starts_with(R"({"id":")" + entry.config.id + R"(","workload":")" + entry.config.workload + R"(","params":{)"));
        CHECK(line.find('\n') == std::string::npos);
        CHECK(ids.insert(entry.config.id).second);
        CHECK(find(entry.config.id) == &entry);
        CHECK(entry.runner(Arm::Rojo) != nullptr);
        CHECK(entry.runner(Arm::Native) != nullptr);
    }
    CHECK(find("C0-k1") == nullptr);
    CHECK(listJson(find("C5-k100000")->config) == R"({"id":"C5-k100000","workload":"C5","params":{"K":100000},"arms":["R","N"]})");
    for (const std::string_view workload : { "C5", "C7" })
    {
        for (const std::string_view k : { "1", "10", "100", "1000", "10000", "100000" })
        {
            CHECK(ids.contains(std::string(workload) + "-k" + std::string(k)));
        }
    }
}

//======================================================================================================================
TEST_CASE("the feedback collector spans a frame from its earliest start to its latest end", "[host]")
{
    FeedbackCollector collector;
    collector.begin(3);
    std::thread first([&] { collector.report(10.0, 10.5, false); });
    std::thread second([&] { collector.report(9.75, 10.25, false); });
    first.join();
    second.join();
    collector.report(10.0, 11.0, false);
    const auto span = collector.wait(std::chrono::seconds(1));
    REQUIRE(span.has_value());
    CHECK(*span == 1.25e9);
    CHECK(collector.commits().size() == 3);

    // The collector is reused frame after frame; a new frame forgets the last one's commits.
    collector.begin(1);
    collector.report(1.0, 1.000001, false);
    const auto next = collector.wait(std::chrono::seconds(1));
    REQUIRE(next.has_value());
    CHECK(*next > 999.0);
    CHECK(*next < 1001.0);
    CHECK(collector.commits().size() == 1);
}

//======================================================================================================================
TEST_CASE("the feedback collector reports a failed commit, a timeout and an unexpected commit", "[host]")
{
    FeedbackCollector collector;
    collector.begin(2);
    collector.report(1.0, 2.0, true);
    collector.report(1.0, 2.0, false);
    const auto failed = collector.wait(std::chrono::seconds(1));
    REQUIRE(!failed.has_value());
    CHECK(failed.error().find("failed") != std::string::npos);

    collector.begin(2);
    collector.report(1.0, 2.0, false);
    const auto timedOut = collector.wait(std::chrono::milliseconds(20));
    REQUIRE(!timedOut.has_value());
    CHECK(timedOut.error().find("1 of 2") != std::string::npos);

    collector.begin(1);
    collector.report(1.0, 2.0, false);
    collector.report(1.0, 2.0, false);
    const auto extra = collector.wait(std::chrono::seconds(1));
    REQUIRE(!extra.has_value());
    CHECK(extra.error().find("2 commits reported, 1 expected") != std::string::npos);
}

//======================================================================================================================
TEST_CASE("a commit report outside its frame fails the frame that sees it and every later one", "[host]")
{
    FeedbackCollector late;
    late.begin(1);
    const uint64_t first = late.frame();
    late.report(first, 1.0, 2.0, false);
    REQUIRE(late.wait(std::chrono::seconds(1)).has_value());
    // A report while no frame is open belongs to none.
    late.report(first, 1.0, 2.0, false);
    late.begin(1);
    late.report(late.frame(), 1.0, 2.0, false);
    const auto between = late.wait(std::chrono::seconds(1));
    REQUIRE(!between.has_value());
    CHECK(between.error().find("outside the frame") != std::string::npos);
    late.begin(1);
    late.report(1.0, 2.0, false);
    CHECK(!late.wait(std::chrono::seconds(1)).has_value());

    FeedbackCollector stale;
    stale.begin(1);
    const uint64_t previous = stale.frame();
    stale.report(previous, 1.0, 2.0, false);
    REQUIRE(stale.wait(std::chrono::seconds(1)).has_value());
    stale.begin(2);
    CHECK(stale.expectedSoFar() == 3);
    stale.report(previous, 1.0, 2.0, false);
    stale.report(stale.frame(), 1.0, 2.0, false);
    stale.report(stale.frame(), 1.0, 2.0, false);
    const auto tagged = stale.wait(std::chrono::seconds(1));
    REQUIRE(!tagged.has_value());
    CHECK(tagged.error().find("1 commit reports arrived outside") != std::string::npos);
}

//======================================================================================================================
TEST_CASE("RojoRHI's commit times map to the open frame by queue and serial", "[host]")
{
    using rojoRHI::QueueType;
    FeedbackCollector collector;
    RojoGpuTiming timing(collector);
    // Frame 1: two graphics commits and one compute commit, reported out of order.
    collector.begin(3);
    timing.report(QueueType::Compute, 1, 1.0, 2.0, false);
    timing.report(QueueType::Graphics, 2, 1.5, 3.0, false);
    timing.report(QueueType::Graphics, 1, 0.5, 1.0, false);
    REQUIRE(collector.wait(std::chrono::seconds(1)) == 2.5e9);
    REQUIRE(timing.closeFrame().has_value());
    // Frame 2 continues each queue's serials.
    collector.begin(2);
    timing.report(QueueType::Graphics, 3, 1.0, 2.0, false);
    timing.report(QueueType::Compute, 2, 1.0, 2.0, false);
    REQUIRE(collector.wait(std::chrono::seconds(1)).has_value());
    CHECK(timing.closeFrame().has_value());
}

//======================================================================================================================
TEST_CASE("a late RojoRHI commit time cannot stand in for a missing one of the open frame", "[host]")
{
    using rojoRHI::QueueType;
    FeedbackCollector collector;
    RojoGpuTiming timing(collector);
    collector.begin(1);
    timing.report(QueueType::Graphics, 1, 1.0, 2.0, false);
    REQUIRE(collector.wait(std::chrono::seconds(1)).has_value());
    REQUIRE(timing.closeFrame().has_value());
    // Frame 2 expects graphics serials 2 and 3; serial 1 again belongs to the closed frame.
    collector.begin(2);
    timing.report(QueueType::Graphics, 1, 1.0, 2.0, false);
    timing.report(QueueType::Graphics, 2, 1.0, 2.0, false);
    const auto waited = collector.wait(std::chrono::seconds(1));
    REQUIRE(!waited.has_value());
    CHECK(waited.error().find("outside the frame") != std::string::npos);
}

//======================================================================================================================
TEST_CASE("a frame whose RojoRHI commit serials skip one fails when it closes", "[host]")
{
    using rojoRHI::QueueType;
    FeedbackCollector collector;
    RojoGpuTiming timing(collector);
    // Serial 2 never reports; serial 3, which belongs to a later commit, completes the count.
    collector.begin(2);
    timing.report(QueueType::Copy, 1, 1.0, 2.0, false);
    timing.report(QueueType::Copy, 3, 1.0, 2.0, false);
    REQUIRE(collector.wait(std::chrono::seconds(1)).has_value());
    const auto closed = timing.closeFrame();
    REQUIRE(!closed.has_value());
    CHECK(closed.error().find("serial 3") != std::string::npos);

    // A commit time before any frame begins belongs to none.
    FeedbackCollector early;
    RojoGpuTiming before(early);
    before.report(QueueType::Graphics, 1, 1.0, 2.0, false);
    early.begin(0);
    CHECK(!early.wait(std::chrono::seconds(1)).has_value());
}

//======================================================================================================================
TEST_CASE("an output's SHA-256 matches the standard's test vectors", "[host]")
{
    CHECK(sha256Hex(bytesOf("abc")) == kAbcSha256);
    CHECK(sha256Hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

//======================================================================================================================
TEST_CASE("dumped outputs land in one file per arm and output, holding the output's bytes", "[host]")
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("rojorhi-bench-dumps-" + std::to_string(getpid())) / "nested";
    const std::array<Output, 2> outputs{ Output{ "dst", bytesOf("abc") }, Output{ "count", bytesOf("") } };
    REQUIRE(dumpOutputs(directory, "C7-k100", Arm::Native, outputs).has_value());
    std::ifstream file(directory / "C7-k100.N.dst.bin", std::ios::binary);
    const std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    file.close();
    CHECK(text == "abc");
    CHECK(std::filesystem::file_size(directory / "C7-k100.N.count.bin") == 0);
    std::filesystem::remove_all(directory.parent_path());
}
