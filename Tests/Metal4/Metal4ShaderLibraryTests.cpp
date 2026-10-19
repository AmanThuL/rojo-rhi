//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4ShaderLibraryTests.cpp
/// @brief Tests loading RojoRHI shader libraries on the Metal 4 backend: the container rules and the checks against the metallib.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Pipelines.h"
#include "Core/Records.h"
#include "Metal4Device.h"
#include "Metal4ShaderLibrary.h"
#include "ShaderList.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// Reads `file` whole.
    std::vector<std::byte> readFile(const std::filesystem::path& file)
    {
        std::ifstream stream(file, std::ios::binary);
        std::vector<char> chars((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const auto* bytes = reinterpret_cast<const std::byte*>(chars.data());
        return { bytes, bytes + chars.size() };
    }

    //==================================================================================================================
    /// The bytes of the metallib inside `Shaders/AbiCompute.metal4lib`.
    std::vector<std::byte> computePayload()
    {
        const std::filesystem::path file = std::filesystem::path("Shaders") / "AbiCompute.metal4lib";
        REQUIRE(std::filesystem::exists(file));
        const std::vector<std::byte> blob = readFile(file);
        const std::string_view text(reinterpret_cast<const char*>(blob.data()), blob.size());
        const size_t at = text.find("MTLB");
        REQUIRE(at != std::string_view::npos);
        return { blob.begin() + static_cast<std::ptrdiff_t>(at), blob.end() };
    }

    //==================================================================================================================
    /// Returns `header` followed by `payload` as one blob.
    std::vector<std::byte> blobOf(std::string_view header, const std::vector<std::byte>& payload)
    {
        const auto* bytes = reinterpret_cast<const std::byte*>(header.data());
        std::vector<std::byte> blob(bytes, bytes + header.size());
        blob.insert(blob.end(), payload.begin(), payload.end());
        return blob;
    }

    //==================================================================================================================
    /// Returns a header with the ABI stamp `abi`, the entry lines `entries` and the declared byte count `metallib`.
    std::string header(std::string_view abi, std::string_view entries, std::string_view metallib)
    {
        return std::format("rojorhi.metal4.shaderlibrary\nabi {}\n{}metallib {}\n", abi, entries, metallib);
    }

    //==================================================================================================================
    std::string goodAbi()
    {
        return std::to_string(core::kShaderAbiVersion);
    }

    //==================================================================================================================
    /// Checks that loading `blob` fails with `category` and a message containing `text`.
    void checkRefused(Device& device, const std::vector<std::byte>& blob, ErrorCategory category, std::string_view text)
    {
        const Result<ShaderLibrary> result = device.loadShaderLibrary(blob, "test.library");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().category == category);
        CHECK_THAT(result.error().message, Catch::Matchers::ContainsSubstring(std::string(text)));
    }

    //==================================================================================================================
    /// Checks that a header with `entries` and the payload's real byte count is refused with `text` in the message.
    void checkEntries(Device& device, std::string_view entries, std::string_view text)
    {
        const std::vector<std::byte> payload = computePayload();
        checkRefused(device, blobOf(header(goodAbi(), entries, std::to_string(payload.size())), payload), ErrorCategory::DriverFailure, text);
    }

    const char* kCompute = "entry computeMain compute 1 1 1\n";

    //==================================================================================================================
    std::vector<std::byte> bytesOf(std::string_view text)
    {
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        return { bytes, bytes + text.size() };
    }
} // namespace

//======================================================================================================================
TEST_CASE("every conformance shader library loads with the entries, stages and sizes of its header", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    for (const std::string_view name : kConformanceShaders)
    {
        INFO(name);
        const std::vector<std::byte> blob = readFile(std::filesystem::path("Shaders") / (std::string(name) + ".metal4lib"));
        const Result<metal4::LibraryHeader> parsed = metal4::parseShaderLibrary(blob);
        REQUIRE(parsed.has_value());
        const Result<ShaderLibrary> library = device.loadShaderLibrary(blob, "test.conformance");
        REQUIRE(library.has_value());
        const core::ShaderLibraryRecord& record = *DeviceImpl::of(device).core.pool<ShaderLibrary>().checked(*library, false);
        const auto* payload = static_cast<const metal4::LibraryPayload*>(record.native);
        REQUIRE(payload != nullptr);
        REQUIRE(record.entries.size() == parsed->entries.size());
        REQUIRE(payload->sizes.size() == parsed->entries.size());
        for (size_t i = 0; i < record.entries.size(); ++i)
        {
            CHECK(record.entries[i].name == parsed->entries[i].name);
            CHECK(record.entries[i].stage == parsed->entries[i].stage);
            CHECK(payload->sizes[i] == parsed->entries[i].size);
        }
        if (name == "AbiCompute")
        {
            REQUIRE(record.entries.size() == 1);
            CHECK(record.entries[0].name == "computeMain");
            CHECK(record.entries[0].stage == core::ShaderStage::Compute);
            CHECK(payload->sizes[0] == std::array<uint32_t, 3>{ 1, 1, 1 });
        }
        if (name == "AbiMesh")
        {
            REQUIRE(record.entries.size() == 2);
            CHECK(record.entries[0].name == "taskMain");
            CHECK(payload->sizes[0] == std::array<uint32_t, 3>{ 1, 1, 1 });
            CHECK(record.entries[1].name == "meshMain");
            CHECK(payload->sizes[1] == std::array<uint32_t, 3>{ 3, 1, 1 });
        }
        device.destroy(*library);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a library stamped with another ABI version is ShaderAbiMismatch, even with a malformed entry after the stamp", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    const std::vector<std::byte> payload = computePayload();
    checkRefused(
        device,
        blobOf(header(std::to_string(core::kShaderAbiVersion + 1), kCompute, std::to_string(payload.size())), payload),
        ErrorCategory::ShaderAbiMismatch,
        std::format("ABI version {}", core::kShaderAbiVersion + 1)
    );
    for (const std::string_view other : { "0", "4294967296", "18446744073709551615" })
    {
        checkRefused(device, blobOf(header(other, "entry broken\n", "7"), payload), ErrorCategory::ShaderAbiMismatch, std::format("ABI version {}", other));
    }
    checkRefused(
        device,
        blobOf(header(std::to_string(core::kShaderAbiVersion + 1), "entry broken\n", "7"), payload),
        ErrorCategory::ShaderAbiMismatch,
        std::format("needs {}", core::kShaderAbiVersion)
    );
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a bare metallib and another backend's blob are DriverFailure, not built for Metal 4", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    checkRefused(device, computePayload(), ErrorCategory::DriverFailure, "not built for the Metal 4 backend");
    checkRefused(device, bytesOf("rojorhi.null.shaderlibrary\nabi 1\nentry main compute\n"), ErrorCategory::DriverFailure, "not built for the Metal 4 backend");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a header that breaks a reading rule is DriverFailure naming its line", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    const std::vector<std::byte> payload = computePayload();
    const std::string size = std::to_string(payload.size());

    checkRefused(
        device, blobOf(std::format("rojorhi.metal4.shaderlibrary\nabi 01\n{}metallib {}\n", kCompute, size), payload), ErrorCategory::DriverFailure, "line 2"
    );
    checkRefused(
        device, blobOf(std::format("rojorhi.metal4.shaderlibrary\nversion 1\n{}metallib {}\n", kCompute, size), payload), ErrorCategory::DriverFailure, "line 2"
    );
    checkEntries(device, "entry computeMain compute 1 1\n", "line 3");
    checkEntries(device, "entry computeMain geometry 1 1 1\n", "unknown stage");
    checkEntries(device, "entry computeMain compute 01 1 1\n", "canonical");
    checkEntries(device, "entry computeMain compute 0 1 1\n", "line 3");
    checkEntries(device, "entry vertexMain vertex 1 0 0\n", "0 0 0");
    checkEntries(device, "entry computeMain compute 1 1 1\nentry computeMain compute 1 1 1\n", "line 4");
    checkEntries(device, "", "line 3");
    checkRefused(
        device,
        blobOf(std::format("rojorhi.metal4.shaderlibrary\nabi {}\n{}", goodAbi(), kCompute), {}),
        ErrorCategory::DriverFailure,
        "has no `metallib <bytes>` line"
    );
    checkRefused(device, blobOf(header(goodAbi(), kCompute, std::to_string(payload.size() + 1)), payload), ErrorCategory::DriverFailure, "line 4");
    checkRefused(device, blobOf(header(goodAbi(), kCompute, std::to_string(payload.size() - 1)), payload), ErrorCategory::DriverFailure, "line 4");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an empty or foreign metallib passes the header and is refused by Metal", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    checkRefused(device, bytesOf(header(goodAbi(), kCompute, "0")), ErrorCategory::DriverFailure, "Metal refused");
    const std::vector<std::byte> junk = bytesOf("not a metallib at all");
    checkRefused(device, blobOf(header(goodAbi(), kCompute, std::to_string(junk.size())), junk), ErrorCategory::DriverFailure, "Metal refused");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an entry the metallib does not hold, or holds as another stage, is DriverFailure naming it", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    const std::vector<std::byte> payload = computePayload();
    const std::string size = std::to_string(payload.size());
    checkRefused(
        device,
        blobOf(header(goodAbi(), std::string(kCompute) + "entry missingMain compute 1 1 1\n", size), payload),
        ErrorCategory::DriverFailure,
        "missingMain"
    );
    checkRefused(
        device,
        blobOf(header(goodAbi(), "entry computeMain vertex 0 0 0\n", size), payload),
        ErrorCategory::DriverFailure,
        "declared vertex (Metal function type"
    );
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a failed load creates nothing to destroy", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    core::DeviceCore& core = DeviceImpl::of(device).core;
    const size_t before = core.pool<ShaderLibrary>().liveCount();
    const std::vector<std::byte> payload = computePayload();
    CHECK_FALSE(device.loadShaderLibrary(blobOf(header(goodAbi(), "entry missingMain compute 1 1 1\n", std::to_string(payload.size())), payload), "test.bad"));
    CHECK_FALSE(device.loadShaderLibrary(computePayload(), "test.bare"));
    CHECK(core.pool<ShaderLibrary>().liveCount() == before);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a destroyed library is released when its frame retires", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({ .framesInFlight = 2 });
    core::DeviceCore& core = DeviceImpl::of(device).core;
    const size_t before = core.pool<ShaderLibrary>().liveCount();
    const std::vector<std::byte> payload = computePayload();
    const ShaderLibrary library = *device.loadShaderLibrary(blobOf(header(goodAbi(), kCompute, std::to_string(payload.size())), payload), "test.retire");
    CHECK(core.pool<ShaderLibrary>().liveCount() == before + 1);
    // The test holds its own reference, so the library's retain count shows when the retire hook releases the payload's.
    MTL::Library* native = static_cast<metal4::LibraryPayload*>(core.pool<ShaderLibrary>().native(library))->library;
    native->retain();
    CHECK(native->retainCount() == 2);
    device.destroy(library);
    for (int frame = 0; frame < 3; ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    CHECK(core.pool<ShaderLibrary>().liveCount() == before);
    CHECK(native->retainCount() == 1);
    native->release();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("four threads load libraries at once", "[metal4]")
{
    REQUIRE(kMetalLibrariesBuilt);
    Device device = *createDevice({});
    const std::vector<std::byte> payload = computePayload();
    const std::vector<std::byte> blob = blobOf(header(goodAbi(), kCompute, std::to_string(payload.size())), payload);
    std::array<std::vector<ShaderLibrary>, 4> loaded;
    std::vector<std::thread> threads;
    for (size_t t = 0; t < loaded.size(); ++t)
    {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 8; ++i)
            {
                const Result<ShaderLibrary> library = device.loadShaderLibrary(blob, "test.threads");
                if (library)
                {
                    loaded[t].push_back(*library);
                }
            }
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    for (const std::vector<ShaderLibrary>& libraries : loaded)
    {
        CHECK(libraries.size() == 8);
        for (const ShaderLibrary library : libraries)
        {
            device.destroy(library);
        }
    }
    destroyDevice(device);
}
