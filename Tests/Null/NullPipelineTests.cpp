//----------------------------------------------------------------------------------------------------------------------
/// @file NullPipelineTests.cpp
/// @brief Tests what only the Null backend promises about shader libraries and the pipeline cache: their blob formats.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/Pipelines.h"
#include "Support/Backend.h"
#include "Support/Null/NullLibrary.h"

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    std::vector<std::byte> bytesOf(std::string_view text)
    {
        const auto* data = reinterpret_cast<const std::byte*>(text.data());
        return { data, data + text.size() };
    }

    //==================================================================================================================
    std::string textOf(const std::vector<std::byte>& blob)
    {
        return { reinterpret_cast<const char*>(blob.data()), blob.size() };
    }
} // namespace

//======================================================================================================================
TEST_CASE("a Null library of the magic, the ABI stamp and no entries loads", "[null]")
{
    Device device = *createDevice();
    const std::string text = std::format("rojorhi.null.shaderlibrary\nabi {}\n", core::kShaderAbiVersion);
    CHECK(device.loadShaderLibrary(bytesOf(text), "null.library").has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null library of another magic is a DriverFailure", "[null]")
{
    Device device = *createDevice();
    const Result<ShaderLibrary> library = device.loadShaderLibrary(test::nullLibrary({}, core::kShaderAbiVersion, "metallib"), "null.library");
    REQUIRE_FALSE(library.has_value());
    CHECK(library.error().category == ErrorCategory::DriverFailure);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null library with no ABI line is a DriverFailure", "[null]")
{
    Device device = *createDevice();
    const Result<ShaderLibrary> library = device.loadShaderLibrary(bytesOf("rojorhi.null.shaderlibrary\n"), "null.library");
    REQUIRE_FALSE(library.has_value());
    CHECK(library.error().category == ErrorCategory::DriverFailure);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null library with an unknown stage is a DriverFailure", "[null]")
{
    Device device = *createDevice();
    const Result<ShaderLibrary> library = device.loadShaderLibrary(test::nullLibrary({ { "main", "geometry" } }), "null.library");
    REQUIRE_FALSE(library.has_value());
    CHECK(library.error().category == ErrorCategory::DriverFailure);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null library of ABI version 0 or one above the current is a ShaderAbiMismatch", "[null]")
{
    Device device = *createDevice();
    for (uint32_t abi : { 0u, core::kShaderAbiVersion + 1 })
    {
        const Result<ShaderLibrary> library = device.loadShaderLibrary(test::nullLibrary({ { "main", "compute" } }, abi), "null.library");
        REQUIRE_FALSE(library.has_value());
        CHECK(library.error().category == ErrorCategory::ShaderAbiMismatch);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the Null pipeline cache blob names the Null backend and the pipelines it covers", "[null]")
{
    Device device = *createDevice();
    CHECK(textOf(*device.pipelineCacheData()) == "rojorhi.null.pipelinecache\nversion 1\npipelines 0\n");
    const std::vector<std::byte> blob = test::standardLibrary();
    const ShaderLibrary library = *device.loadShaderLibrary(blob, "null.library");
    REQUIRE(device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" } }).has_value());
    REQUIRE(device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" } }).has_value());
    CHECK(textOf(*device.pipelineCacheData()) == "rojorhi.null.pipelinecache\nversion 1\npipelines 2\n");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null device carries the pipelines of the cache blob it was created with", "[null]")
{
    const std::vector<std::byte> cache = bytesOf("rojorhi.null.pipelinecache\nversion 1\npipelines 7\n");
    Device device = *createDevice(DeviceDesc{ .pipelineCache = cache });
    CHECK(textOf(*device.pipelineCacheData()) == "rojorhi.null.pipelinecache\nversion 1\npipelines 7\n");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null device discards a cache blob of another version and starts empty", "[null]")
{
    const std::vector<std::byte> cache = bytesOf("rojorhi.null.pipelinecache\nversion 2\npipelines 7\n");
    Device device = *createDevice(DeviceDesc{ .pipelineCache = cache });
    CHECK(textOf(*device.pipelineCacheData()) == "rojorhi.null.pipelinecache\nversion 1\npipelines 0\n");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null library that repeats an entry name is a DriverFailure", "[null]")
{
    Device device = *createDevice();
    for (const auto& entries : { std::vector<test::LibraryEntry>{ { "main", "vertex" }, { "main", "vertex" } },
                                 std::vector<test::LibraryEntry>{ { "main", "vertex" }, { "main", "fragment" } } })
    {
        const Result<ShaderLibrary> library = device.loadShaderLibrary(test::nullLibrary(entries), "null.library");
        REQUIRE_FALSE(library.has_value());
        CHECK(library.error().category == ErrorCategory::DriverFailure);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a malformed Null library is an error and never a crash", "[null]")
{
    Device device = *createDevice();
    const std::string magic = "rojorhi.null.shaderlibrary";
    const std::vector<std::string> blobs{
        magic,
        magic + "\nabi \n",
        magic + "\nabi 1x\n",
        magic + "\nabi 99999999999999999999999999\n",
        std::format("{}\nabi {}\nentry  vertex\n", magic, core::kShaderAbiVersion),
        std::format("{}\nabi {}\nmain vertex\n", magic, core::kShaderAbiVersion),
        std::string(1 << 20, '\n'),
        std::string(1 << 20, '\xFF'),
    };
    for (const std::string& text : blobs)
    {
        const Result<ShaderLibrary> library = device.loadShaderLibrary(bytesOf(text), "null.library");
        CHECK_FALSE(library.has_value());
    }
    destroyDevice(device);
}
