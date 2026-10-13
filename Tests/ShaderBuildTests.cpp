#include "ShaderList.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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

//======================================================================================================================
// An SPIR-V module is a 5-word header, then instructions whose first word is (word count << 16) | opcode.
static bool declaresCapability(const std::filesystem::path& module, std::uint32_t capability)
{
    constexpr std::uint32_t kOpCapability = 17;
    constexpr std::size_t kHeaderWords = 5;
    std::ifstream file(module, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::copy_n(bytes.data(), words.size() * sizeof(std::uint32_t), reinterpret_cast<char*>(words.data()));
    for (std::size_t at = kHeaderWords; at < words.size();)
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
TEST_CASE("SPIR-V heap fetches carry the NonUniform capability", "[abi]")
{
    constexpr std::uint32_t kShaderNonUniform = 5301;
    CHECK(declaresCapability("Shaders/AbiTextures.spv", kShaderNonUniform));
}
