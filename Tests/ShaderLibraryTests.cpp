//----------------------------------------------------------------------------------------------------------------------
/// @file ShaderLibraryTests.cpp
/// @brief Tests the header of the RojoRHI shader library the shader build writes around each conformance metallib.
//----------------------------------------------------------------------------------------------------------------------
#include "Core/Pipelines.h"
#include "ShaderList.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    constexpr std::string_view kMagicLine = "rojorhi.metal4.shaderlibrary";

    /// One `entry` line of a library header.
    struct LibraryEntry
    {
        std::string name;                  ///< The entry point's name.
        std::string stage;                 ///< The stage word, as `core::stageName` spells it.
        std::array<uint64_t, 3> size = {}; ///< The threadgroup size; zero for vertex and fragment.

        /// Compares two entries.
        friend bool operator==(const LibraryEntry&, const LibraryEntry&) = default;
    };

    /// What the header of a library declares, read up to and including its `metallib` line.
    struct LibraryHeader
    {
        uint64_t abi = 0;                  ///< The value of the `abi` line.
        std::vector<LibraryEntry> entries; ///< The `entry` lines, in file order.
        uint64_t declaredBytes = 0;        ///< The count of the `metallib` line.
        std::vector<char> payload;         ///< The bytes after the `metallib` line, to the end of the file.
        std::string error;                 ///< "line <n>: <breach>" for the first breach; empty when there is none.
    };

    //==================================================================================================================
    std::vector<char> readFile(const std::filesystem::path& file)
    {
        std::ifstream stream(file, std::ios::binary);
        return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
    }

    //==================================================================================================================
    // A canonical unsigned decimal: digits only, no sign, no leading zero except for 0 itself.
    std::optional<uint64_t> canonicalNumber(std::string_view text)
    {
        if (text.empty() || (text.size() > 1 && text.front() == '0'))
        {
            return std::nullopt;
        }
        uint64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc() || end != text.data() + text.size())
        {
            return std::nullopt;
        }
        return value;
    }

    //==================================================================================================================
    bool validName(std::string_view name)
    {
        const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
        const auto digit = [](char c) { return c >= '0' && c <= '9'; };
        if (name.empty() || !letter(name.front()))
        {
            return false;
        }
        for (const char c : name)
        {
            if (!letter(c) && !digit(c))
            {
                return false;
            }
        }
        return true;
    }

    //==================================================================================================================
    std::vector<std::string_view> splitSpaces(std::string_view line)
    {
        std::vector<std::string_view> tokens;
        for (std::size_t first = 0;;)
        {
            const std::size_t space = line.find(' ', first);
            tokens.push_back(line.substr(first, space == std::string_view::npos ? std::string_view::npos : space - first));
            if (space == std::string_view::npos)
            {
                return tokens;
            }
            first = space + 1;
        }
    }

    //==================================================================================================================
    // Reads an `entry <name> <stage> <x> <y> <z>` line into `header`, or returns the breach.
    std::string readEntry(std::string_view line, LibraryHeader& header, std::set<std::string, std::less<>>& names)
    {
        const std::vector<std::string_view> tokens = splitSpaces(line);
        if (tokens.size() != 6)
        {
            return std::format("an entry line holds {} tokens, not 6", tokens.size());
        }
        LibraryEntry entry;
        entry.name = tokens[1];
        entry.stage = tokens[2];
        if (!validName(entry.name))
        {
            return std::format("entry name '{}' is not [A-Za-z_][A-Za-z0-9_]*", entry.name);
        }
        if (!names.insert(entry.name).second)
        {
            return std::format("entry '{}' is repeated", entry.name);
        }
        bool known = false;
        for (const core::ShaderStage stage :
             { core::ShaderStage::Vertex, core::ShaderStage::Fragment, core::ShaderStage::Task, core::ShaderStage::Mesh, core::ShaderStage::Compute })
        {
            known = known || core::stageName(stage) == entry.stage;
        }
        if (!known)
        {
            return std::format("entry '{}' has unknown stage '{}'", entry.name, entry.stage);
        }
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const std::optional<uint64_t> value = canonicalNumber(tokens[3 + axis]);
            if (!value)
            {
                return std::format("entry '{}' size '{}' is not a canonical number", entry.name, tokens[3 + axis]);
            }
            entry.size[axis] = *value;
        }
        const bool sized = entry.stage == "compute" || entry.stage == "task" || entry.stage == "mesh";
        for (const uint64_t extent : entry.size)
        {
            if (sized ? extent == 0 : extent != 0)
            {
                return std::format("{} entry '{}' size {} {} {} breaks the size rule", entry.stage, entry.name, entry.size[0], entry.size[1], entry.size[2]);
            }
        }
        header.entries.push_back(std::move(entry));
        return {};
    }

    //==================================================================================================================
    // Reads LF-ended header lines up to and including `metallib <n>`, never the payload as text.
    LibraryHeader readHeader(std::span<const char> bytes)
    {
        LibraryHeader header;
        std::set<std::string, std::less<>> names;
        std::size_t at = 0;
        for (std::size_t number = 1;; ++number)
        {
            const auto fail = [&](std::string breach) {
                header.error = std::format("line {}: {}", number, breach);
                return header;
            };
            const std::string_view rest(bytes.data() + at, bytes.size() - at);
            const std::size_t end = rest.find('\n');
            if (end == std::string_view::npos)
            {
                return fail("the header ends before a metallib line");
            }
            const std::string_view line = rest.substr(0, end);
            at += end + 1;
            if (line.find('\r') != std::string_view::npos)
            {
                return fail("the line holds a carriage return");
            }
            if (number == 1)
            {
                if (line != kMagicLine)
                {
                    return fail(std::format("the first line is not '{}'", kMagicLine));
                }
                continue;
            }
            if (number == 2)
            {
                const std::optional<uint64_t> abi = line.starts_with("abi ") ? canonicalNumber(line.substr(4)) : std::nullopt;
                if (!abi)
                {
                    return fail("the second line is not 'abi <number>'");
                }
                header.abi = *abi;
                continue;
            }
            if (line.starts_with("entry "))
            {
                std::string breach = readEntry(line, header, names);
                if (!breach.empty())
                {
                    return fail(std::move(breach));
                }
                continue;
            }
            if (!line.starts_with("metallib "))
            {
                return fail(std::format("'{}' is neither an entry line nor the metallib line", line));
            }
            if (header.entries.empty())
            {
                return fail("the metallib line follows no entry line");
            }
            const std::optional<uint64_t> count = canonicalNumber(line.substr(9));
            if (!count)
            {
                return fail("the metallib line holds no canonical byte count");
            }
            header.declaredBytes = *count;
            header.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
            if (header.declaredBytes != header.payload.size())
            {
                return fail(std::format("the metallib line declares {} bytes but {} follow", header.declaredBytes, header.payload.size()));
            }
            return header;
        }
    }

    //==================================================================================================================
    // Catch2 prints a failed comparison of entries through this.
    std::ostream& operator<<(std::ostream& stream, const LibraryEntry& entry)
    {
        return stream << std::format("{} {} {} {} {}", entry.name, entry.stage, entry.size[0], entry.size[1], entry.size[2]);
    }

    //==================================================================================================================
    // Reads the header of a conformance shader's library and fails the case on a breach. xmake runs a test from its target
    // directory, which is where the build wrote Shaders/.
    LibraryHeader readLibrary(std::string_view shader)
    {
        const auto file = std::filesystem::path("Shaders") / (std::string(shader) + ".metal4lib");
        INFO(file.string());
        REQUIRE(std::filesystem::exists(file));
        LibraryHeader header = readHeader(readFile(file));
        INFO(header.error);
        REQUIRE(header.error.empty());
        return header;
    }
} // namespace

//======================================================================================================================
TEST_CASE("every conformance shader packages a Metal shader library", "[abi]")
{
    if (!kMetalLibrariesBuilt)
    {
        SKIP("the build had no Metal toolchain");
    }
    for (const std::string_view name : kConformanceShaders)
    {
        INFO(name);
        const LibraryHeader header = readLibrary(name);
        CHECK(header.abi == core::kShaderAbiVersion);
        CHECK_FALSE(header.entries.empty());
        CHECK(header.declaredBytes == header.payload.size());
        REQUIRE(header.payload.size() >= 4);
        CHECK(std::string_view(header.payload.data(), 4) == "MTLB");
    }
}

//======================================================================================================================
TEST_CASE("the Metal shader libraries carry each entry's stage and threadgroup size", "[abi]")
{
    if (!kMetalLibrariesBuilt)
    {
        SKIP("the build had no Metal toolchain");
    }
    using Entries = std::vector<LibraryEntry>;
    CHECK(readLibrary("AbiMesh").entries == Entries{ { "taskMain", "task", { 1, 1, 1 } }, { "meshMain", "mesh", { 3, 1, 1 } } });
    CHECK(readLibrary("AbiVertex").entries == Entries{ { "vertexMain", "vertex", { 0, 0, 0 } } });
    CHECK(readLibrary("AbiFragment").entries == Entries{ { "fragmentMain", "fragment", { 0, 0, 0 } } });
    CHECK(readLibrary("AbiCompute").entries == Entries{ { "computeMain", "compute", { 1, 1, 1 } } });

    // One kernel per texture type: seven sampled, the comparison sample, two multisample and five storage.
    const Entries textures = readLibrary("AbiTextures").entries;
    CHECK(textures.size() == 15);
    for (const LibraryEntry& entry : textures)
    {
        INFO(entry.name);
        CHECK(entry.stage == "compute");
        CHECK(entry.size == std::array<uint64_t, 3>{ 1, 1, 1 });
    }
}
