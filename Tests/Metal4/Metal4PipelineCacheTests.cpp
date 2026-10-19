//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4PipelineCacheTests.cpp
/// @brief Tests the Metal 4 pipeline cache: blobs carried between devices, hits and misses, refused blobs and temporary files.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The one color target the render and mesh pipelines draw to.
    constexpr std::array<ColorTarget, 1> kColor = { ColorTarget{ .format = Format::RGBA8Unorm } };

    /// The number of lines of a blob's header.
    constexpr size_t kHeaderLines = 6;

    std::mutex gMessageMutex;           ///< Guards `gWarnings`; the callback may run on any thread.
    std::vector<std::string> gWarnings; ///< The warnings `deviceWarnings` collected.

    //==================================================================================================================
    /// Collects the warnings delivered to the message callback.
    void collectWarning(MessageSeverity severity, std::string_view message, void*)
    {
        if (severity == MessageSeverity::Warning)
        {
            const std::lock_guard lock(gMessageMutex);
            gWarnings.emplace_back(message);
        }
    }

    //==================================================================================================================
    /// Loads the standard library on `device`.
    ShaderLibrary loadStandard(Device& device)
    {
        const Result<ShaderLibrary> library = device.loadShaderLibrary(test::standardLibrary(), "test.standard");
        REQUIRE(library.has_value());
        return *library;
    }

    //==================================================================================================================
    /// Creates the standard vertex and fragment pipeline, failing the case when creation fails.
    Pipeline createRender(Device& device, ShaderLibrary library)
    {
        const Result<Pipeline> pipeline = device.createPipeline(
            GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .colors = kColor, .label = "test.render" }
        );
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    //==================================================================================================================
    /// Creates the standard task, mesh and fragment pipeline, failing the case when creation fails.
    Pipeline createMesh(Device& device, ShaderLibrary library)
    {
        const Result<Pipeline> pipeline = device.createPipeline(
            GraphicsPipelineDesc{
                .task = { library, "tsMain" }, .mesh = { library, "msMain" }, .fragment = { library, "fsMain" }, .colors = kColor, .label = "test.mesh" }
        );
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    //==================================================================================================================
    /// Creates the standard compute pipeline, failing the case when creation fails.
    Pipeline createCompute(Device& device, ShaderLibrary library)
    {
        const Result<Pipeline> pipeline = device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" }, .label = "test.compute" });
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    //==================================================================================================================
    /// Returns the cache blob of `device`, failing the case when it cannot be serialized.
    std::vector<std::byte> harvest(Device& device)
    {
        Result<std::vector<std::byte>> data = device.pipelineCacheData();
        INFO((data ? std::string() : data.error().message));
        REQUIRE(data.has_value());
        return std::move(*data);
    }

    //==================================================================================================================
    /// Creates a device with `blob` as its pipeline cache, failing the case when creation fails.
    Device deviceWith(std::span<const std::byte> blob)
    {
        Result<Device> device = createDevice(DeviceDesc{ .pipelineCache = blob });
        REQUIRE(device.has_value());
        return *device;
    }

    //==================================================================================================================
    /// Creates a device with `blob` as its pipeline cache, failing the case when creation fails, and writes the warnings
    /// its creation delivered to `warnings`.
    Device deviceWarnings(std::span<const std::byte> blob, std::vector<std::string>& warnings)
    {
        {
            const std::lock_guard lock(gMessageMutex);
            gWarnings.clear();
        }
        setMessageCallback(collectWarning, nullptr);
        Result<Device> device = createDevice(DeviceDesc{ .pipelineCache = blob });
        setMessageCallback(nullptr, nullptr);
        REQUIRE(device.has_value());
        const std::lock_guard lock(gMessageMutex);
        warnings = gWarnings;
        return *device;
    }

    //==================================================================================================================
    /// A blob of a device that created the standard render pipeline.
    std::vector<std::byte> renderBlob()
    {
        Device device = *createDevice();
        createRender(device, loadStandard(device));
        std::vector<std::byte> blob = harvest(device);
        destroyDevice(device);
        return blob;
    }

    //==================================================================================================================
    /// The header lines of `blob`, without their line feeds, and the offset of the bytes after them.
    std::pair<std::vector<std::string>, size_t> headerOf(const std::vector<std::byte>& blob)
    {
        const std::string_view text(reinterpret_cast<const char*>(blob.data()), blob.size());
        std::vector<std::string> lines;
        size_t at = 0;
        while (lines.size() < kHeaderLines)
        {
            const size_t end = text.find('\n', at);
            REQUIRE(end != std::string_view::npos);
            lines.emplace_back(text.substr(at, end - at));
            at = end + 1;
        }
        return { lines, at };
    }

    //==================================================================================================================
    /// Returns `lines`, each followed by a line feed, and then `payload`.
    std::vector<std::byte> joined(const std::vector<std::string>& lines, std::span<const std::byte> payload)
    {
        std::string header;
        for (const std::string& line : lines)
        {
            header += line;
            header += '\n';
        }
        const auto* bytes = reinterpret_cast<const std::byte*>(header.data());
        std::vector<std::byte> blob(bytes, bytes + header.size());
        blob.insert(blob.end(), payload.begin(), payload.end());
        return blob;
    }

    //==================================================================================================================
    /// Returns `blob` with header line `index` replaced by `line`.
    std::vector<std::byte> withLine(const std::vector<std::byte>& blob, size_t index, std::string line)
    {
        auto [lines, at] = headerOf(blob);
        lines[index] = std::move(line);
        return joined(lines, std::span(blob).subspan(at));
    }

    //==================================================================================================================
    /// The number of `archive <bytes>` sections in `blob`, found by walking the byte counts after the header.
    size_t archiveLines(const std::vector<std::byte>& blob)
    {
        const std::string_view text(reinterpret_cast<const char*>(blob.data()), blob.size());
        size_t count = 0;
        size_t at = text.find("\narchive ") + 1;
        while (at < text.size())
        {
            const size_t end = text.find('\n', at);
            REQUIRE(end != std::string_view::npos);
            REQUIRE(text.substr(at).starts_with("archive "));
            at = end + 1 + std::stoull(std::string(text.substr(at + 8, end - at - 8)));
            ++count;
        }
        return count;
    }

    //==================================================================================================================
    /// Returns `blob` followed by the text `tail` and then `payload`.
    std::vector<std::byte> withTail(std::vector<std::byte> blob, std::string_view tail, std::span<const std::byte> payload = {})
    {
        const auto* bytes = reinterpret_cast<const std::byte*>(tail.data());
        blob.insert(blob.end(), bytes, bytes + tail.size());
        blob.insert(blob.end(), payload.begin(), payload.end());
        return blob;
    }

    //==================================================================================================================
    /// The per-user temporary directory of the process, which the backend's temporary files live in.
    std::filesystem::path userTemporaryDirectory()
    {
        std::array<char, 1024> path = {};
        const size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, path.data(), path.size());
        REQUIRE(length > 0);
        REQUIRE(length <= path.size());
        return std::filesystem::path(path.data());
    }

    //==================================================================================================================
    /// The entries of the temporary directory this process made, named `rojorhi-pipelines-<pid>-*`.
    std::vector<std::string> temporaryFiles()
    {
        const std::string prefix = std::format("rojorhi-pipelines-{}-", getpid());
        std::vector<std::string> files;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(userTemporaryDirectory()))
        {
            const std::string name = entry.path().filename().string();
            if (name.starts_with(prefix))
            {
                files.push_back(name);
            }
        }
        // The directory lists in no set order, so the names are sorted for comparison.
        std::ranges::sort(files);
        return files;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a second device created with the first's blob counts a hit for each pipeline", "[metal4]")
{
    Device first = *createDevice();
    const ShaderLibrary library = loadStandard(first);
    createRender(first, library);
    createMesh(first, library);
    createCompute(first, library);
    const Counters created = first.counters();
    CHECK(created.pipelineCacheHits == 0);
    CHECK(created.pipelineCacheMisses == 3);
    const std::vector<std::byte> blob = harvest(first);
    destroyDevice(first);

    std::vector<std::string> warnings;
    Device second = deviceWarnings(blob, warnings);
    CHECK(warnings.empty());
    const ShaderLibrary again = loadStandard(second);
    createRender(second, again);
    createMesh(second, again);
    createCompute(second, again);
    const Counters counters = second.counters();
    CHECK(counters.pipelinesCreated == 3);
    CHECK(counters.pipelineCacheHits == 3);
    CHECK(counters.pipelineCacheMisses == 0);
    destroyDevice(second);
}

//======================================================================================================================
TEST_CASE("a blob covers the pipelines it served as hits", "[metal4]")
{
    const std::vector<std::byte> first = renderBlob();

    Device second = deviceWith(first);
    createRender(second, loadStandard(second));
    REQUIRE(second.counters().pipelineCacheHits == 1);
    const std::vector<std::byte> blob = harvest(second);
    destroyDevice(second);

    std::vector<std::string> warnings;
    Device third = deviceWarnings(blob, warnings);
    CHECK(warnings.empty());
    createRender(third, loadStandard(third));
    CHECK(third.counters().pipelineCacheHits == 1);
    CHECK(third.counters().pipelineCacheMisses == 0);
    destroyDevice(third);
}

//======================================================================================================================
TEST_CASE("a second pipelineCacheData on one device still covers the pipelines the first covered", "[metal4]")
{
    Device first = *createDevice();
    const ShaderLibrary library = loadStandard(first);
    createRender(first, library);
    const std::vector<std::byte> early = harvest(first);
    createCompute(first, library);
    const std::vector<std::byte> late = harvest(first);
    // A harvest with no pipeline created since the last still covers both.
    const std::vector<std::byte> idle = harvest(first);
    destroyDevice(first);

    // The idle harvest is the late blob again, with one archive section for each serialization.
    CHECK(idle == late);
    CHECK(archiveLines(early) == 1);
    CHECK(archiveLines(late) == 2);

    // The first blob holds the render pipeline alone.
    Device second = deviceWith(early);
    const ShaderLibrary again = loadStandard(second);
    createRender(second, again);
    createCompute(second, again);
    CHECK(second.counters().pipelineCacheHits == 1);
    CHECK(second.counters().pipelineCacheMisses == 1);
    destroyDevice(second);

    // The later ones hold both, though the first serialization came between them.
    for (const std::vector<std::byte>* bytes : { &late, &idle })
    {
        std::vector<std::string> warnings;
        Device third = deviceWarnings(*bytes, warnings);
        CHECK(warnings.empty());
        const ShaderLibrary last = loadStandard(third);
        createRender(third, last);
        createCompute(third, last);
        CHECK(third.counters().pipelineCacheHits == 2);
        CHECK(third.counters().pipelineCacheMisses == 0);
        destroyDevice(third);
    }
}

//======================================================================================================================
TEST_CASE("a pipeline the blob lacks is a miss", "[metal4]")
{
    Device device = deviceWith(renderBlob());
    const ShaderLibrary library = loadStandard(device);
    createRender(device, library);
    createCompute(device, library);
    createMesh(device, library);
    const Counters counters = device.counters();
    CHECK(counters.pipelineCacheHits == 1);
    CHECK(counters.pipelineCacheMisses == 2);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a blob of another RojoRHI version, OS, device or format is one warning and every pipeline a miss", "[metal4]")
{
    const std::vector<std::byte> blob = renderBlob();
    const auto [lines, at] = headerOf(blob);
    REQUIRE(lines[0] == "rojorhi.metal4.pipelinecache");
    REQUIRE(lines[1] == "version 1");
    REQUIRE(lines[2] == std::format("rojorhi {}.{}.{}", ROJORHI_VERSION_MAJOR, ROJORHI_VERSION_MINOR, ROJORHI_VERSION_PATCH));
    REQUIRE(lines[3].starts_with("device "));
    REQUIRE(lines[4].starts_with("os "));
    REQUIRE(lines[5] == std::format("archive {}", blob.size() - at));

    const std::string_view null = "rojorhi.null.pipelinecache\nversion 1\npipelines 2\n";
    const auto* nullBytes = reinterpret_cast<const std::byte*>(null.data());
    const std::array<std::byte, 4> garbage = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 }, std::byte{ 4 } };
    const std::span<const std::byte> archive = std::span(blob).subspan(at);
    std::vector<std::byte> truncated = blob;
    truncated.resize(truncated.size() - 16);
    std::vector<std::string> recounted = lines;
    recounted[5] = std::format("archive {}", archive.size() / 2);

    const std::vector<std::pair<std::string, std::vector<std::byte>>> refused = {
        { "another format", withLine(blob, 0, "rojorhi.metal4.pipelinecachf") },
        { "another format version", withLine(blob, 1, "version 2") },
        { "another RojoRHI version", withLine(blob, 2, "rojorhi 99.0.0") },
        { "another device", withLine(blob, 3, "device Another GPU") },
        { "another OS", withLine(blob, 4, "os Version 1.0 (Build 1A1)") },
        { "a wrong byte count", withLine(blob, 5, std::format("archive {}", archive.size() + 1)) },
        { "a Null blob", std::vector<std::byte>(nullBytes, nullBytes + null.size()) },
        { "garbage", std::vector<std::byte>(garbage.begin(), garbage.end()) },
        { "a truncated archive", truncated },
        { "half an archive, counted", joined(recounted, archive.first(archive.size() / 2)) },
    };
    for (const auto& [name, bytes] : refused)
    {
        INFO(name);
        std::vector<std::string> warnings;
        Device device = deviceWarnings(bytes, warnings);
        CHECK(warnings.size() == 1);
        createRender(device, loadStandard(device));
        CHECK(device.counters().pipelineCacheHits == 0);
        CHECK(device.counters().pipelineCacheMisses == 1);
        destroyDevice(device);
    }
}

//======================================================================================================================
TEST_CASE("a malformed second section is one warning and every pipeline a miss", "[metal4]")
{
    const std::vector<std::byte> good = renderBlob();
    const std::array<std::byte, 16> garbage = {};
    const std::vector<std::pair<std::string, std::vector<std::byte>>> refused = {
        { "a count past the end", withTail(good, "archive 999999\n", garbage) },
        { "a count that is no number", withTail(good, "archive many\n", garbage) },
        { "a count without its line feed", withTail(good, "archive 16") },
        { "trailing text", withTail(good, "trailing text") },
        { "a damaged second archive", withTail(good, std::format("archive {}\n", garbage.size()), garbage) },
    };
    const std::vector<std::string> before = temporaryFiles();
    for (const auto& [name, bytes] : refused)
    {
        INFO(name);
        std::vector<std::string> warnings;
        Device device = deviceWarnings(bytes, warnings);
        CHECK(warnings.size() == 1);
        // The good first archive was opened and is released with its file, so nothing stays behind.
        CHECK(temporaryFiles() == before);
        createRender(device, loadStandard(device));
        CHECK(device.counters().pipelineCacheHits == 0);
        CHECK(device.counters().pipelineCacheMisses == 1);
        destroyDevice(device);
    }
}

//======================================================================================================================
TEST_CASE("each malformed form of a blob has its own warning", "[metal4]")
{
    const std::vector<std::byte> good = renderBlob();
    const std::array<std::byte, 16> garbage = {};
    const std::vector<std::vector<std::byte>> forms = {
        withTail(good, "archive 999999\n", garbage),
        withTail(good, "trailing text"),
        withTail(good, std::format("archive {}\n", garbage.size()), garbage),
        withLine(good, 4, "os Version 1.0 (Build 1A1)"),
    };
    std::vector<std::string> seen;
    for (const std::vector<std::byte>& bytes : forms)
    {
        std::vector<std::string> warnings;
        Device device = deviceWarnings(bytes, warnings);
        destroyDevice(device);
        REQUIRE(warnings.size() == 1);
        seen.push_back(warnings[0]);
    }
    std::ranges::sort(seen);
    CHECK(std::ranges::adjacent_find(seen) == seen.end());
}

//======================================================================================================================
TEST_CASE("pipelineCacheData runs beside createPipeline on other threads", "[metal4]")
{
    Device device = deviceWith(renderBlob());
    const ShaderLibrary library = loadStandard(device);
    std::atomic<int> running = 4;
    std::atomic<int> failures = 0;
    std::vector<std::thread> creators;
    for (int t = 0; t < 4; ++t)
    {
        creators.emplace_back([&, t] {
            for (int i = 0; i < 6; ++i)
            {
                const Result<Pipeline> pipeline =
                    (i + t) % 2 == 0 ? device.createPipeline(
                                           GraphicsPipelineDesc{
                                               .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .colors = kColor, .label = "test.render" }
                                       )
                                     : device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" }, .label = "test.compute" });
                if (!pipeline)
                {
                    ++failures;
                }
            }
            --running;
        });
    }
    int harvests = 0;
    int harvestFailures = 0;
    do
    {
        harvestFailures += device.pipelineCacheData().has_value() ? 0 : 1;
        ++harvests;
    } while (running.load() > 0);
    for (std::thread& creator : creators)
    {
        creator.join();
    }
    CHECK(failures.load() == 0);
    CHECK(harvestFailures == 0);
    CHECK(harvests > 0);
    const Counters counters = device.counters();
    CHECK(counters.pipelinesCreated == 24);
    CHECK(counters.pipelineCacheHits == 12);
    CHECK(counters.pipelineCacheMisses == 12);
    const std::vector<std::byte> blob = harvest(device);
    destroyDevice(device);

    std::vector<std::string> warnings;
    Device later = deviceWarnings(blob, warnings);
    CHECK(warnings.empty());
    const ShaderLibrary again = loadStandard(later);
    createRender(later, again);
    createCompute(later, again);
    CHECK(later.counters().pipelineCacheHits == 2);
    destroyDevice(later);
}

//======================================================================================================================
TEST_CASE("a device that created no pipeline returns a blob a later device accepts", "[metal4]")
{
    Device first = *createDevice();
    const std::vector<std::byte> blob = harvest(first);
    destroyDevice(first);

    std::vector<std::string> warnings;
    Device second = deviceWarnings(blob, warnings);
    CHECK(warnings.empty());
    createRender(second, loadStandard(second));
    CHECK(second.counters().pipelineCacheHits == 0);
    CHECK(second.counters().pipelineCacheMisses == 1);
    destroyDevice(second);
}

//======================================================================================================================
TEST_CASE("two devices alive at once keep their own caches", "[metal4]")
{
    const std::vector<std::byte> blob = renderBlob();
    Device cached = deviceWith(blob);
    Device plain = *createDevice();
    createRender(plain, loadStandard(plain));
    createRender(cached, loadStandard(cached));
    CHECK(plain.counters().pipelineCacheHits == 0);
    CHECK(cached.counters().pipelineCacheHits == 1);
    const std::vector<std::byte> fromPlain = harvest(plain);
    const std::vector<std::byte> fromCached = harvest(cached);
    destroyDevice(plain);
    destroyDevice(cached);
    for (const std::vector<std::byte>* bytes : { &fromPlain, &fromCached })
    {
        Device later = deviceWith(*bytes);
        createRender(later, loadStandard(later));
        CHECK(later.counters().pipelineCacheHits == 1);
        destroyDevice(later);
    }
}

//======================================================================================================================
TEST_CASE("no temporary file is left behind", "[metal4]")
{
    const std::vector<std::string> before = temporaryFiles();
    const std::vector<std::byte> blob = renderBlob();
    {
        Device device = deviceWith(blob);
        createRender(device, loadStandard(device));
        CHECK(device.counters().pipelineCacheHits == 1);
        // The archive's file lives as long as the device, because the compiler reads it at each look-up.
        CHECK(temporaryFiles().size() == before.size() + 1);
        destroyDevice(device);
    }
    {
        const auto [lines, at] = headerOf(blob);
        std::vector<std::string> recounted = lines;
        const std::span<const std::byte> archive = std::span(blob).subspan(at);
        recounted[5] = std::format("archive {}", archive.size() / 2);
        std::vector<std::string> warnings;
        Device refused = deviceWarnings(joined(recounted, archive.first(archive.size() / 2)), warnings);
        CHECK(warnings.size() == 1);
        destroyDevice(refused);
    }
    CHECK(temporaryFiles() == before);
}

//======================================================================================================================
TEST_CASE("counters report hits and misses as creations served or not", "[metal4]")
{
    Device device = deviceWith(renderBlob());
    const ShaderLibrary library = loadStandard(device);
    createRender(device, library);
    createRender(device, library);
    createCompute(device, library);
    const Counters counters = device.counters();
    CHECK(counters.pipelinesCreated == 3);
    CHECK(counters.pipelineCacheHits == 2);
    CHECK(counters.pipelineCacheMisses == counters.pipelinesCreated - counters.pipelineCacheHits);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a pipeline compiled again with rasterization the other way is a hit", "[metal4]")
{
    // A vertex entry that returns a position with nothing to rasterize is refused without rasterization and compiled again
    // with it, so the hit is the second compile's.
    const auto vertexOnly = [](Device& device) {
        const ShaderLibrary library = loadStandard(device);
        const Result<Pipeline> pipeline =
            device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .colors = kColor, .label = "test.vsOnly" });
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
    };
    Device first = *createDevice();
    vertexOnly(first);
    const std::vector<std::byte> blob = harvest(first);
    destroyDevice(first);

    Device second = deviceWith(blob);
    vertexOnly(second);
    CHECK(second.counters().pipelineCacheHits == 1);
    CHECK(second.counters().pipelineCacheMisses == 0);
    destroyDevice(second);
}

//======================================================================================================================
TEST_CASE("an archive whose file disappeared is one warning and every later pipeline a miss", "[metal4]")
{
    const std::vector<std::byte> blob = renderBlob();
    const std::vector<std::string> before = temporaryFiles();
    Device device = deviceWith(blob);
    std::vector<std::string> added;
    std::ranges::set_difference(temporaryFiles(), before, std::back_inserter(added));
    const bool oneFile = added.size() == 1;
    const bool removed = oneFile && std::filesystem::remove(userTemporaryDirectory() / added[0]);
    if (!removed)
    {
        destroyDevice(device);
    }
    REQUIRE(oneFile);
    REQUIRE(removed);

    const ShaderLibrary library = loadStandard(device);
    {
        const std::lock_guard lock(gMessageMutex);
        gWarnings.clear();
    }
    setMessageCallback(collectWarning, nullptr);
    createRender(device, library);
    createRender(device, library);
    setMessageCallback(nullptr, nullptr);
    {
        const std::lock_guard lock(gMessageMutex);
        CHECK(gWarnings.size() == 1);
    }
    const Counters counters = device.counters();
    CHECK(counters.pipelineCacheHits == 0);
    CHECK(counters.pipelineCacheMisses == 2);
    destroyDevice(device);
}
