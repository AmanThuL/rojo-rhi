//----------------------------------------------------------------------------------------------------------------------
/// @file ShaderLibraryTests.cpp
/// @brief Tests the RojoRHI shader library containers, the Null libraries the build writes and the embedded library.
//----------------------------------------------------------------------------------------------------------------------
#include "Core/Pipelines.h"
#include "EmbeddedShaders.h"
#include "ShaderList.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
    // A production reader lives in each backend; the readers here only check what the build wrote.

    /// What distinguishes the two library formats.
    struct LibraryFormat
    {
        std::string_view magic;        ///< The first line.
        std::string_view extension;    ///< The file extension.
        std::string_view closing;      ///< The first word of the last header line, a payload byte count follows it.
        std::size_t entryTokens;       ///< The fields of an `entry` line, the word included.
        std::string_view payloadMagic; ///< The four bytes that start the metallib, or each entry's DXIL.
    };

    constexpr LibraryFormat kMetal = { "rojorhi.metal4.shaderlibrary", ".metal4lib", "metallib", 6, "MTLB" };
    constexpr LibraryFormat kD3D12 = { "rojorhi.d3d12.shaderlibrary", ".d3d12lib", "dxil", 7, "DXBC" };

    /// One `entry` line of a library header.
    struct LibraryEntry
    {
        std::string name;                  ///< The entry point's name.
        std::string stage;                 ///< The stage word, as `core::stageName` spells it.
        std::array<uint64_t, 3> size = {}; ///< The threadgroup size; zero for vertex and fragment.
        uint64_t bytes = 0;                ///< The DXIL byte count of a D3D12 entry line; zero for Metal, which has none.

        /// Compares two entries by name, stage and size; the byte count is read, not compared.
        friend bool operator==(const LibraryEntry& left, const LibraryEntry& right)
        {
            return left.name == right.name && left.stage == right.stage && left.size == right.size;
        }
    };

    /// What the header of a library declares, read up to and including its closing line.
    struct LibraryHeader
    {
        uint64_t abi = 0;                  ///< The value of the `abi` line.
        std::vector<LibraryEntry> entries; ///< The `entry` lines, in file order.
        uint64_t declaredBytes = 0;        ///< The count of the closing line.
        std::vector<char> payload;         ///< The bytes after the closing line, to the end of the file.
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
    bool knownStage(std::string_view stage)
    {
        bool known = false;
        for (const core::ShaderStage candidate :
             { core::ShaderStage::Vertex, core::ShaderStage::Fragment, core::ShaderStage::Task, core::ShaderStage::Mesh, core::ShaderStage::Compute })
        {
            known = known || core::stageName(candidate) == stage;
        }
        return known;
    }

    //==================================================================================================================
    // Reads an `entry <name> <stage> <x> <y> <z>` line, with a trailing byte count in a D3D12 library, into `header`, or
    // returns the breach.
    std::string readEntry(const LibraryFormat& format, std::string_view line, LibraryHeader& header, std::set<std::string, std::less<>>& names)
    {
        const std::vector<std::string_view> tokens = splitSpaces(line);
        if (tokens.size() != format.entryTokens)
        {
            return std::format("an entry line holds {} tokens, not {}", tokens.size(), format.entryTokens);
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
        if (!knownStage(entry.stage))
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
        if (format.entryTokens == 7)
        {
            const std::optional<uint64_t> bytes = canonicalNumber(tokens[6]);
            if (!bytes || *bytes == 0)
            {
                return std::format("entry '{}' byte count '{}' is not a canonical nonzero number", entry.name, tokens[6]);
            }
            entry.bytes = *bytes;
        }
        header.entries.push_back(std::move(entry));
        return {};
    }

    //==================================================================================================================
    // Reads LF-ended header lines up to and including the closing `metallib <n>` or `dxil <n>`, never the payload as text.
    LibraryHeader readHeader(const LibraryFormat& format, std::span<const char> bytes)
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
                return fail(std::format("the header ends before the {} line", format.closing));
            }
            const std::string_view line = rest.substr(0, end);
            at += end + 1;
            if (line.find('\r') != std::string_view::npos)
            {
                return fail("the line holds a carriage return");
            }
            if (number == 1)
            {
                if (line != format.magic)
                {
                    return fail(std::format("the first line is not '{}'", format.magic));
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
                std::string breach = readEntry(format, line, header, names);
                if (!breach.empty())
                {
                    return fail(std::move(breach));
                }
                continue;
            }
            const std::string closing = std::format("{} ", format.closing);
            if (!line.starts_with(closing))
            {
                return fail(std::format("'{}' is neither an entry line nor the {} line", line, format.closing));
            }
            if (header.entries.empty())
            {
                return fail(std::format("the {} line follows no entry line", format.closing));
            }
            const std::optional<uint64_t> count = canonicalNumber(line.substr(closing.size()));
            if (!count)
            {
                return fail(std::format("the {} line holds no canonical byte count", format.closing));
            }
            header.declaredBytes = *count;
            header.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
            if (header.declaredBytes != header.payload.size())
            {
                return fail(std::format("the {} line declares {} bytes but {} follow", format.closing, header.declaredBytes, header.payload.size()));
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
    LibraryHeader readLibrary(const LibraryFormat& format, std::string_view shader)
    {
        const auto file = std::filesystem::path("Shaders") / (std::string(shader) + std::string(format.extension));
        INFO(file.string());
        REQUIRE(std::filesystem::exists(file));
        LibraryHeader header = readHeader(format, readFile(file));
        INFO(header.error);
        REQUIRE(header.error.empty());
        return header;
    }

    using Entries = std::vector<LibraryEntry>;

    /// A conformance shader whose entries are known in full.
    struct ExpectedLibrary
    {
        std::string_view shader; ///< The entry file's basename.
        Entries entries;         ///< Its entries, in the order of the reflection.
    };

    // One kernel per texture type of AbiTextures: seven sampled, the comparison sample, two multisample and five storage.
    constexpr std::size_t kTextureEntries = 15;

    //==================================================================================================================
    std::vector<ExpectedLibrary> expectedLibraries()
    {
        return {
            { "AbiMesh", { { "taskMain", "task", { 1, 1, 1 } }, { "meshMain", "mesh", { 3, 1, 1 } } } },
            { "AbiVertex", { { "vertexMain", "vertex", { 0, 0, 0 } } } },
            { "AbiFragment", { { "fragmentMain", "fragment", { 0, 0, 0 } } } },
            { "AbiCompute", { { "computeMain", "compute", { 1, 1, 1 } } } },
            { "Standard",
              { { "vsMain", "vertex", { 0, 0, 0 } },
                { "fsMain", "fragment", { 0, 0, 0 } },
                { "tsMain", "task", { 1, 1, 1 } },
                { "msMain", "mesh", { 1, 1, 1 } },
                { "csMain", "compute", { 1, 1, 1 } } } },
            { "SharedImport", { { "sharedMain", "compute", { 1, 1, 1 } } } },
        };
    }

    //==================================================================================================================
    // Checks the entry lists of every conformance shader's library in `format`; both formats carry the same ones.
    void checkEntryLists(const LibraryFormat& format)
    {
        for (const ExpectedLibrary& expected : expectedLibraries())
        {
            INFO(expected.shader);
            CHECK(readLibrary(format, expected.shader).entries == expected.entries);
        }

        const Entries textures = readLibrary(format, "AbiTextures").entries;
        CHECK(textures.size() == kTextureEntries);
        for (const LibraryEntry& entry : textures)
        {
            INFO(entry.name);
            CHECK(entry.stage == "compute");
            CHECK(entry.size == std::array<uint64_t, 3>{ 1, 1, 1 });
        }
    }

    /// One `entry <name> <stage>` line of a Null library.
    struct NullEntry
    {
        std::string name;  ///< The entry point's name.
        std::string stage; ///< The stage word, as `core::stageName` spells it.

        /// Compares two entries by name and stage.
        friend bool operator==(const NullEntry&, const NullEntry&) = default;
    };

    //==================================================================================================================
    // Catch2 prints a failed comparison of Null entries through this.
    std::ostream& operator<<(std::ostream& stream, const NullEntry& entry)
    {
        return stream << std::format("{} {}", entry.name, entry.stage);
    }

    //==================================================================================================================
    // Reads the LF-ended lines of a conformance shader's Null library, failing the case on a missing file, a missing final
    // line feed or a carriage return.
    std::vector<std::string> readNullLines(std::string_view shader)
    {
        const auto file = std::filesystem::path("Shaders") / (std::string(shader) + ".nulllib");
        INFO(file.string());
        REQUIRE(std::filesystem::exists(file));
        const std::vector<char> bytes = readFile(file);
        const std::string_view text(bytes.data(), bytes.size());
        REQUIRE(text.ends_with('\n'));
        REQUIRE(text.find('\r') == std::string_view::npos);
        std::vector<std::string> lines;
        for (std::size_t first = 0; first < text.size();)
        {
            const std::size_t end = text.find('\n', first);
            lines.emplace_back(text.substr(first, end - first));
            first = end + 1;
        }
        return lines;
    }

    //==================================================================================================================
    // The entries of a conformance shader's Null library, in file order.
    std::vector<NullEntry> readNullEntries(std::string_view shader)
    {
        const std::vector<std::string> lines = readNullLines(shader);
        std::vector<NullEntry> entries;
        for (std::size_t i = 2; i < lines.size(); ++i)
        {
            const std::vector<std::string_view> tokens = splitSpaces(lines[i]);
            REQUIRE(tokens.size() == 3);
            entries.push_back({ std::string(tokens[1]), std::string(tokens[2]) });
        }
        return entries;
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
        const LibraryHeader header = readLibrary(kMetal, name);
        CHECK(header.abi == core::kShaderAbiVersion);
        CHECK_FALSE(header.entries.empty());
        CHECK(header.declaredBytes == header.payload.size());
        REQUIRE(header.payload.size() >= 4);
        CHECK(std::string_view(header.payload.data(), 4) == kMetal.payloadMagic);
    }
}

//======================================================================================================================
TEST_CASE("the Metal shader libraries carry each entry's stage and threadgroup size", "[abi]")
{
    if (!kMetalLibrariesBuilt)
    {
        SKIP("the build had no Metal toolchain");
    }
    checkEntryLists(kMetal);
}

//======================================================================================================================
TEST_CASE("every conformance shader packages a D3D12 shader library", "[abi]")
{
    if (!kD3D12LibrariesBuilt)
    {
        SKIP("the build had no DXC");
    }
    for (const std::string_view name : kConformanceShaders)
    {
        INFO(name);
        const LibraryHeader header = readLibrary(kD3D12, name);
        CHECK(header.abi == core::kShaderAbiVersion);
        CHECK_FALSE(header.entries.empty());
        CHECK(header.declaredBytes == header.payload.size());
        uint64_t sum = 0;
        std::size_t at = 0;
        for (const LibraryEntry& entry : header.entries)
        {
            INFO(entry.name);
            sum += entry.bytes;
            REQUIRE(at + 4 <= header.payload.size());
            CHECK(std::string_view(header.payload.data() + at, 4) == kD3D12.payloadMagic);
            at += entry.bytes;
        }
        CHECK(sum == header.payload.size());
    }
}

//======================================================================================================================
TEST_CASE("the D3D12 shader libraries carry each entry's stage, threadgroup size and DXIL", "[abi]")
{
    if (!kD3D12LibrariesBuilt)
    {
        SKIP("the build had no DXC");
    }
    checkEntryLists(kD3D12);
    for (const std::string_view name : kConformanceShaders)
    {
        INFO(name);
        for (const LibraryEntry& entry : readLibrary(kD3D12, name).entries)
        {
            INFO(entry.name);
            CHECK(entry.bytes > 0);
        }
    }
}

//======================================================================================================================
TEST_CASE("each entry's DXIL in a D3D12 library is the rule's .dxil for that entry", "[abi]")
{
    if (!kD3D12LibrariesBuilt)
    {
        SKIP("the build had no DXC");
    }
    for (const std::string_view name : kConformanceShaders)
    {
        INFO(name);
        const LibraryHeader header = readLibrary(kD3D12, name);
        std::size_t at = 0;
        for (const LibraryEntry& entry : header.entries)
        {
            INFO(entry.name);
            const std::string base(name);
            const auto single = std::filesystem::path("Shaders") / (base + ".dxil");
            const auto file = header.entries.size() == 1 ? single : std::filesystem::path("Shaders") / (base + "." + entry.name + ".dxil");
            REQUIRE(std::filesystem::exists(file));
            const std::vector<char> expected = readFile(file);
            REQUIRE(at + entry.bytes <= header.payload.size());
            CHECK(
                std::equal(
                    expected.begin(),
                    expected.end(),
                    header.payload.begin() + static_cast<std::ptrdiff_t>(at),
                    header.payload.begin() + static_cast<std::ptrdiff_t>(at + entry.bytes)
                )
            );
            at += entry.bytes;
        }
    }
}

//======================================================================================================================
TEST_CASE("every conformance shader packages a Null shader library", "[abi]")
{
    STATIC_REQUIRE(kNullLibrariesBuilt);
    for (const std::string_view name : kConformanceShaders)
    {
        INFO(name);
        const std::vector<std::string> lines = readNullLines(name);
        REQUIRE(lines.size() >= 3);
        CHECK(lines[0] == "rojorhi.null.shaderlibrary");
        CHECK(lines[1] == std::format("abi {}", core::kShaderAbiVersion));
        std::set<std::string, std::less<>> names;
        for (std::size_t i = 2; i < lines.size(); ++i)
        {
            INFO(lines[i]);
            const std::vector<std::string_view> tokens = splitSpaces(lines[i]);
            REQUIRE(tokens.size() == 3);
            CHECK(tokens[0] == "entry");
            CHECK(validName(tokens[1]));
            CHECK(names.insert(std::string(tokens[1])).second);
            CHECK(knownStage(tokens[2]));
        }
    }
}

//======================================================================================================================
TEST_CASE("the Null shader libraries list the entries the Metal and D3D12 libraries carry", "[abi]")
{
    for (const ExpectedLibrary& expected : expectedLibraries())
    {
        INFO(expected.shader);
        std::vector<NullEntry> entries;
        for (const LibraryEntry& entry : expected.entries)
        {
            entries.push_back({ entry.name, entry.stage });
        }
        CHECK(readNullEntries(expected.shader) == entries);
    }

    const std::vector<NullEntry> textures = readNullEntries("AbiTextures");
    CHECK(textures.size() == kTextureEntries);
    for (const NullEntry& entry : textures)
    {
        INFO(entry.name);
        CHECK(entry.stage == "compute");
    }
}

//======================================================================================================================
TEST_CASE("the embedded Standard library is the configured backend's library", "[abi]")
{
#if defined(ROJORHI_TEST_BACKEND_METAL4)
    constexpr std::string_view magic = "rojorhi.metal4.shaderlibrary\n";
#elif defined(ROJORHI_TEST_BACKEND_D3D12)
    constexpr std::string_view magic = "rojorhi.d3d12.shaderlibrary\n";
#elif defined(ROJORHI_TEST_BACKEND_NULL)
    constexpr std::string_view magic = "rojorhi.null.shaderlibrary\n";
#else
#error "the test build defines no ROJORHI_TEST_BACKEND_*"
#endif
    const std::span<const std::byte> library = embedded::kStandardLibrary;
    REQUIRE(library.size() > magic.size());
    const std::string first(reinterpret_cast<const char*>(library.data()), magic.size());
    CHECK(first == magic);
}
