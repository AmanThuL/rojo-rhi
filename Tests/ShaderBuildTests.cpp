#include "ShaderList.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

//======================================================================================================================
// xmake runs a test from its target directory, which is where the build wrote Shaders/.
TEST_CASE("every conformance shader compiles to MSL, SPIR-V and HLSL", "[abi]")
{
    const std::filesystem::path directory = "Shaders";
    for (const std::string_view name : kConformanceShaders)
    {
        for (const char* extension : { ".metal", ".spv", ".hlsl" })
        {
            const auto output = directory / (std::string(name) + extension);
            INFO(output.string());
            CHECK(std::filesystem::exists(output));
        }
    }
}

// An SPIR-V module is a 5-word header, then instructions whose first word is (word count << 16) | opcode.
static constexpr std::size_t kSpirvHeaderWords = 5;

//======================================================================================================================
static std::vector<std::uint32_t> readSpirvWords(const std::filesystem::path& module)
{
    std::ifstream file(module, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::copy_n(bytes.data(), words.size() * sizeof(std::uint32_t), reinterpret_cast<char*>(words.data()));
    return words;
}

//======================================================================================================================
static bool declaresCapability(const std::filesystem::path& module, std::uint32_t capability)
{
    constexpr std::uint32_t kOpCapability = 17;
    const std::vector<std::uint32_t> words = readSpirvWords(module);
    for (std::size_t at = kSpirvHeaderWords; at < words.size();)
    {
        const std::uint32_t count = words[at] >> 16;
        if (count == 0)
        {
            return false;
        }
        if ((words[at] & 0xFFFF) == kOpCapability && at + 1 < words.size() && words[at + 1] == capability)
        {
            return true;
        }
        at += count;
    }
    return false;
}

//======================================================================================================================
// A literal string operand: NUL-terminated UTF-8 packed little-endian into words.
static std::string spirvString(const std::vector<std::uint32_t>& words, std::size_t first, std::size_t end)
{
    std::string text;
    for (std::size_t at = first; at < end; ++at)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            const char character = static_cast<char>((words[at] >> shift) & 0xFF);
            if (character == '\0')
            {
                return text;
            }
            text.push_back(character);
        }
    }
    return text;
}

//======================================================================================================================
TEST_CASE("SPIR-V heap fetches carry the NonUniform capability", "[abi]")
{
    constexpr std::uint32_t kShaderNonUniform = 5301;
    CHECK(declaresCapability("Shaders/AbiTextures.spv", kShaderNonUniform));
}

//======================================================================================================================
TEST_CASE("every SPIR-V heap fetch is decorated NonUniform", "[abi]")
{
    constexpr std::uint32_t kOpName = 5;
    constexpr std::uint32_t kOpEntryPoint = 15;
    constexpr std::uint32_t kOpFunction = 54;
    constexpr std::uint32_t kOpLoad = 61;
    constexpr std::uint32_t kOpAccessChain = 65;
    constexpr std::uint32_t kOpDecorate = 71;
    constexpr std::uint32_t kNonUniform = 5300;

    for (const std::string_view name : kConformanceShaders)
    {
        const auto module = std::filesystem::path("Shaders") / (std::string(name) + ".spv");
        const std::vector<std::uint32_t> words = readSpirvWords(module);
        REQUIRE(words.size() > kSpirvHeaderWords);

        std::set<std::uint32_t> heaps;
        std::set<std::uint32_t> decorated;
        std::vector<std::uint32_t> chains;
        std::vector<std::uint32_t> loads;
        std::set<std::uint32_t> entryFunctions;
        std::map<std::uint32_t, std::size_t> loadsIn;
        std::uint32_t function = 0;
        for (std::size_t at = kSpirvHeaderWords; at < words.size();)
        {
            const std::uint32_t count = words[at] >> 16;
            const std::uint32_t opcode = words[at] & 0xFFFF;
            REQUIRE(count != 0);
            if (opcode == kOpName && count >= 3)
            {
                if (spirvString(words, at + 2, at + count).starts_with("__slang_resource_heap"))
                {
                    heaps.insert(words[at + 1]);
                }
            }
            else if (opcode == kOpDecorate && count >= 3 && words[at + 2] == kNonUniform)
            {
                decorated.insert(words[at + 1]);
            }
            else if (opcode == kOpEntryPoint && count >= 3)
            {
                entryFunctions.insert(words[at + 2]);
            }
            else if (opcode == kOpFunction && count >= 3)
            {
                function = words[at + 2];
            }
            else if (opcode == kOpAccessChain && count >= 5 && heaps.contains(words[at + 3]))
            {
                chains.push_back(words[at + 2]);
            }
            else if (opcode == kOpLoad && count >= 4)
            {
                if (std::ranges::find(chains, words[at + 3]) != chains.end())
                {
                    loads.push_back(words[at + 2]);
                    ++loadsIn[function];
                }
            }
            at += count;
        }

        for (const std::uint32_t id : chains)
        {
            INFO(std::string(name) + ".spv access chain %" + std::to_string(id));
            CHECK(decorated.contains(id));
        }
        for (const std::uint32_t id : loads)
        {
            INFO(std::string(name) + ".spv heap load %" + std::to_string(id));
            CHECK(decorated.contains(id));
        }
        // The files that fetch from the heap must show a fetch, so a stripped name cannot pass vacuously.
        if (name == "AbiTextures" || name == "AbiFragment")
        {
            CHECK(!loads.empty());
        }
        if (name == "AbiTextures")
        {
            CHECK(!entryFunctions.empty());
            for (const std::uint32_t entry : entryFunctions)
            {
                INFO(std::string(name) + ".spv entry function %" + std::to_string(entry));
                CHECK(loadsIn[entry] >= 1);
            }
        }
    }
}
