//----------------------------------------------------------------------------------------------------------------------
/// @file NullBackend.cpp
/// @brief Implements the backend-dependent test support for the Null backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::test {
    namespace {
        /// Stand-ins for native surfaces: the Null backend never dereferences them.
        std::array<int, 8> g_surfaces = {};

        //==============================================================================================================
        /// Reads the library file `path`, failing the running case when it is missing or empty.
        std::vector<std::byte> readLibrary(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                FAIL("the shader library " << std::filesystem::absolute(path) << " is missing");
            }
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (text.empty())
            {
                FAIL("the shader library " << std::filesystem::absolute(path) << " is empty");
            }
            const auto* begin = reinterpret_cast<const std::byte*>(text.data());
            return { begin, begin + text.size() };
        }

        //==============================================================================================================
        /// The packaged standard library, read on first use and kept for the process.
        const std::vector<std::byte>& standardBytes()
        {
            static const std::vector<std::byte> bytes = readLibrary("Shaders/Standard.nulllib");
            return bytes;
        }
    } // namespace

    //==================================================================================================================
    std::vector<std::byte> standardLibrary()
    {
        return standardBytes();
    }

    //==================================================================================================================
    // The library's second line is `abi <N>`; the entry lines after it are kept as they are.
    std::vector<std::byte> libraryStampedWith(uint32_t abi)
    {
        std::vector<std::byte> bytes = standardBytes();
        const auto newline = [&](size_t from) {
            return std::find(bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.end(), std::byte{ '\n' }) - bytes.begin();
        };
        const size_t start = static_cast<size_t>(newline(0)) + 1;
        const size_t end = static_cast<size_t>(newline(start));
        REQUIRE(end < bytes.size());
        const std::string line = "abi " + std::to_string(abi);
        const auto* text = reinterpret_cast<const std::byte*>(line.data());
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.begin() + static_cast<std::ptrdiff_t>(end));
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(start), text, text + line.size());
        return bytes;
    }

    //==================================================================================================================
    std::vector<std::byte> foreignLibrary()
    {
        const std::string_view text = "rojorhi.metal4.shaderlibrary\nabi 1\nentry vsMain vertex 0 0 0\nmetallib 0\n";
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        return { bytes, bytes + text.size() };
    }

    //==================================================================================================================
    // Fails the running case through Catch2, so it is called from a test thread.
    std::vector<std::byte> shaderLibraryFile(std::string_view name)
    {
        return readLibrary(std::filesystem::path("Shaders") / (std::string(name) + ".nulllib"));
    }

    //==================================================================================================================
    // Fails the running case through Catch2, so it is called from a test thread.
    void* surface(uint32_t index)
    {
        if (index >= g_surfaces.size())
        {
            FAIL("surface index " << index << " is not below " << g_surfaces.size());
        }
        return &g_surfaces[index];
    }

    //==================================================================================================================
    bool runsOnGpu()
    {
        return false;
    }

    //==================================================================================================================
    bool multisampleReadsSize()
    {
        return false;
    }

    //==================================================================================================================
    std::string_view misplacedClearEndsRecording()
    {
        return {};
    }

    //==================================================================================================================
    bool layoutsAreNative()
    {
        return false;
    }

    //==================================================================================================================
    std::string_view upscalesNeverComplete()
    {
        return {};
    }
} // namespace rojoRHI::test
