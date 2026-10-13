#include "ShaderList.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

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
