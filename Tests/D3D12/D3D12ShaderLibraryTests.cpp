//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ShaderLibraryTests.cpp
/// @brief Tests parsing RojoRHI shader libraries for the D3D12 backend: the container rules and each entry's DXIL slice.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Pipelines.h"
#include "D3D12Device.h"
#include "D3D12ShaderLibrary.h"
#include "ShaderList.h"
#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    std::vector<std::byte> bytesOf(std::string_view text)
    {
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        return { bytes, bytes + text.size() };
    }

    //==================================================================================================================
    /// A fake DXIL container of `size` bytes: `DXBC`, then `fill` repeated.
    std::string fakeDxil(size_t size, char fill)
    {
        return std::string("DXBC").append(size - 4, fill);
    }

    //==================================================================================================================
    std::string goodAbi()
    {
        return std::to_string(core::kShaderAbiVersion);
    }

    //==================================================================================================================
    /// Returns a library with the ABI stamp `abi`, the entry lines `entries`, the declared total `total` and `payload` after.
    std::vector<std::byte> blobOf(std::string_view abi, std::string_view entries, std::string_view total, std::string_view payload)
    {
        return bytesOf(std::format("rojorhi.d3d12.shaderlibrary\nabi {}\n{}dxil {}\n{}", abi, entries, total, payload));
    }

    //==================================================================================================================
    /// Checks that parsing `blob` fails with `category` and a message containing `text`.
    void checkRefused(const std::vector<std::byte>& blob, ErrorCategory category, std::string_view text)
    {
        const Result<d3d12::LibraryHeader> result = d3d12::parseShaderLibrary(blob);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().category == category);
        CHECK_THAT(result.error().message, Catch::Matchers::ContainsSubstring(std::string(text)));
    }

    //==================================================================================================================
    /// Checks that a library with `entries` over `payload` and the matching total is a `DriverFailure` with `text`.
    void checkEntries(std::string_view entries, std::string_view payload, std::string_view text)
    {
        checkRefused(blobOf(goodAbi(), entries, std::to_string(payload.size()), payload), ErrorCategory::DriverFailure, text);
    }

    const std::string kDxil = fakeDxil(8, 'a');
} // namespace

//======================================================================================================================
TEST_CASE("parseShaderLibrary reads entries, stages, sizes and each entry's DXIL slice", "[d3d12]")
{
    const std::string vertex = fakeDxil(8, 'v');
    const std::string compute = fakeDxil(12, 'c');
    const std::string entries = "entry vsMain vertex 0 0 0 8\nentry csMain compute 8 4 1 12\n";
    const std::vector<std::byte> blob = blobOf(goodAbi(), entries, "20", vertex + compute);
    const Result<d3d12::LibraryHeader> result = d3d12::parseShaderLibrary(blob);
    REQUIRE(result.has_value());
    CHECK(result->abi == core::kShaderAbiVersion);
    REQUIRE(result->entries.size() == 2);
    CHECK(result->entries[0].name == "vsMain");
    CHECK(result->entries[0].stage == core::ShaderStage::Vertex);
    CHECK(result->entries[0].size == std::array<uint32_t, 3>{ 0, 0, 0 });
    CHECK(result->entries[1].name == "csMain");
    CHECK(result->entries[1].stage == core::ShaderStage::Compute);
    CHECK(result->entries[1].size == std::array<uint32_t, 3>{ 8, 4, 1 });
    const auto text = [](std::span<const std::byte> bytes) { return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()); };
    CHECK(text(result->entries[0].dxil) == vertex);
    CHECK(text(result->entries[1].dxil) == compute);
    CHECK(result->entries[0].dxil.data() == blob.data() + blob.size() - 20);
}

//======================================================================================================================
TEST_CASE("a library stamped with another ABI is ShaderAbiMismatch before a malformed entry line", "[d3d12]")
{
    checkRefused(blobOf("2", "entry a vertex 0 0 0 8 extra\n", "8", kDxil), ErrorCategory::ShaderAbiMismatch, "ABI version 2");
}

//======================================================================================================================
TEST_CASE("a Metal 4 library, a Null library and a bare DXIL container are DriverFailure, not built for the D3D12 backend", "[d3d12]")
{
    const std::string_view text = "not built for the D3D12 backend";
    checkRefused(bytesOf("rojorhi.metal4.shaderlibrary\nabi 1\n"), ErrorCategory::DriverFailure, text);
    checkRefused(bytesOf("rojorhi.null.shaderlibrary\nabi 1\nentry vsMain vertex\n"), ErrorCategory::DriverFailure, text);
    checkRefused(bytesOf(kDxil), ErrorCategory::DriverFailure, text);
    checkRefused({}, ErrorCategory::DriverFailure, text);
}

//======================================================================================================================
TEST_CASE("entry byte counts that do not sum to the dxil line are DriverFailure naming the line", "[d3d12]")
{
    checkRefused(blobOf(goodAbi(), "entry a vertex 0 0 0 8\n", "16", kDxil + kDxil), ErrorCategory::DriverFailure, "line 4");
    checkRefused(blobOf(goodAbi(), "entry a vertex 0 0 0 8\n", "16", kDxil + kDxil), ErrorCategory::DriverFailure, "add up to 8");
}

//======================================================================================================================
TEST_CASE("a dxil count past the end of the blob is DriverFailure, and so is one short of it", "[d3d12]")
{
    checkRefused(blobOf(goodAbi(), "entry a vertex 0 0 0 16\n", "16", kDxil), ErrorCategory::DriverFailure, "but 8 follow");
    checkRefused(blobOf(goodAbi(), "entry a vertex 0 0 0 8\n", "8", kDxil + "x"), ErrorCategory::DriverFailure, "but 9 follow");
}

//======================================================================================================================
TEST_CASE("an entry whose bytes do not start DXBC is DriverFailure naming the entry", "[d3d12]")
{
    checkEntries("entry good vertex 0 0 0 8\nentry bad fragment 0 0 0 8\n", kDxil + "DXBX....", "\"bad\"");
    checkEntries("entry tiny vertex 0 0 0 3\n", "DXB", "\"tiny\"");
}

//======================================================================================================================
TEST_CASE(
    "six-token entry lines, an unknown stage, a repeated name, a non-canonical number, a zero compute size and a nonzero "
    "vertex size are each DriverFailure",
    "[d3d12]"
)
{
    checkEntries("entry a vertex 0 0 0\n", kDxil, "line 3: is not `entry");
    checkEntries("entry a geometry 0 0 0 8\n", kDxil, "unknown stage \"geometry\"");
    checkEntries("entry a vertex 0 0 0 4\nentry a vertex 0 0 0 4\n", "DXBCDXBC", "repeats the entry name \"a\"");
    checkEntries("entry a vertex 0 0 0 08\n", kDxil, "not a canonical number");
    checkEntries("entry a compute 0 1 1 8\n", kDxil, "at least 1 on every axis");
    checkEntries("entry a vertex 1 0 0 8\n", kDxil, "to be 0 0 0");
}

//======================================================================================================================
TEST_CASE("no entry line, and no dxil line, are DriverFailure", "[d3d12]")
{
    checkRefused(bytesOf(std::format("rojorhi.d3d12.shaderlibrary\nabi {}\ndxil 0\n", goodAbi())), ErrorCategory::DriverFailure, "no entry line");
    checkRefused(
        bytesOf(std::format("rojorhi.d3d12.shaderlibrary\nabi {}\nentry a vertex 0 0 0 8\n", goodAbi())), ErrorCategory::DriverFailure, "no `dxil <bytes>` line"
    );
}

//======================================================================================================================
TEST_CASE("a malformed or missing second line is DriverFailure naming line 2", "[d3d12]")
{
    checkRefused(bytesOf("rojorhi.d3d12.shaderlibrary\nabi 01\n"), ErrorCategory::DriverFailure, "line 2");
    checkRefused(bytesOf("rojorhi.d3d12.shaderlibrary\nabi\n"), ErrorCategory::DriverFailure, "line 2");
    checkRefused(bytesOf("rojorhi.d3d12.shaderlibrary\n"), ErrorCategory::DriverFailure, "line 2");
}

//======================================================================================================================
TEST_CASE("loadShaderLibrary keeps each entry's DXIL from Standard.d3d12lib and the library retires with its frame", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    DeviceDesc desc;
    desc.framesInFlight = 2;
    Result<Device> created = createDevice(desc);
    REQUIRE(created.has_value());
    core::DeviceCore& core = DeviceImpl::of(*created).core;
    const size_t before = core.pool<ShaderLibrary>().liveCount();
    const std::vector<std::byte> blob = test::standardLibrary();
    const Result<d3d12::LibraryHeader> header = d3d12::parseShaderLibrary(blob);
    REQUIRE(header.has_value());
    const Result<ShaderLibrary> library = created->loadShaderLibrary(blob, "test.library");
    REQUIRE(library.has_value());
    CHECK(core.pool<ShaderLibrary>().liveCount() == before + 1);
    const core::ShaderLibraryRecord* record = core.pool<ShaderLibrary>().lookup(*library).record;
    REQUIRE(record != nullptr);
    REQUIRE(record->entries.size() == header->entries.size());
    const auto* payload = static_cast<const d3d12::LibraryPayload*>(core.pool<ShaderLibrary>().native(*library));
    REQUIRE(payload != nullptr);
    REQUIRE(payload->ranges.size() == header->entries.size());
    for (size_t i = 0; i < header->entries.size(); ++i)
    {
        CHECK(record->entries[i].name == header->entries[i].name);
        CHECK(record->entries[i].stage == header->entries[i].stage);
        const std::span<const std::byte> bytecode = payload->bytecode(i);
        REQUIRE(bytecode.size() == header->entries[i].dxil.size());
        CHECK(std::equal(bytecode.begin(), bytecode.end(), header->entries[i].dxil.begin()));
        // The payload is its own copy: the blob the caller passes is not referenced after the call.
        CHECK(bytecode.data() != header->entries[i].dxil.data());
    }
    const uint64_t retired = created->counters().deferredDestructionsRetired;
    created->destroy(*library);
    for (int frame = 0; frame < 3; ++frame)
    {
        created->beginFrame();
        created->endFrame();
    }
    CHECK(core.pool<ShaderLibrary>().liveCount() == before);
    // Other deferred work may retire in the same frames, so the library is at least one of them.
    CHECK(created->counters().deferredDestructionsRetired >= retired + 1);
    destroyDevice(*created);
}
