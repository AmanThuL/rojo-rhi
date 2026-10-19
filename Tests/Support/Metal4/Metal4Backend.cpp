//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Backend.cpp
/// @brief Implements the backend-dependent test support for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Backend.h"

#include "Metal4Common.h"

#include <catch2/catch_test_macros.hpp>

#include <Foundation/Foundation.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::test {
    namespace {
        /// The layers `surface` hands out, created on first use and retained for the process.
        std::array<CA::MetalLayer*, 8> g_layers = {};

        /// Guards `g_layers`, because cases may ask for a surface from several threads.
        std::mutex g_layersMutex;

        //==============================================================================================================
        /// The packaged standard library, read on first use and kept for the process.
        const std::vector<std::byte>& standardBytes()
        {
            static const std::vector<std::byte> bytes = [] {
                const std::filesystem::path path = "Shaders/Standard.metal4lib";
                std::ifstream file(path, std::ios::binary);
                std::vector<std::byte> contents;
                if (file)
                {
                    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
                    contents.assign(begin, begin + text.size());
                }
                return contents;
            }();
            if (bytes.empty())
            {
                FAIL("the standard library " << std::filesystem::absolute("Shaders/Standard.metal4lib") << " is missing or empty");
            }
            return bytes;
        }
    } // namespace

    //==================================================================================================================
    std::vector<std::byte> standardLibrary()
    {
        return standardBytes();
    }

    //==================================================================================================================
    // The container's second line is `abi <N>`; replacing it leaves the payload byte count unchanged.
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
        const std::string_view text = "rojorhi.null.shaderlibrary\nabi 1\nentry vsMain vertex\n";
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        return { bytes, bytes + text.size() };
    }

    //==================================================================================================================
    // Fails the running case through Catch2, so it is called from a test thread.
    std::vector<std::byte> shaderLibraryFile(std::string_view name)
    {
        const std::filesystem::path path = std::filesystem::path("Shaders") / (std::string(name) + ".metal4lib");
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

    //==================================================================================================================
    // Fails the running case through Catch2, so it is called from a test thread.
    void* surface(uint32_t index)
    {
        if (index >= g_layers.size())
        {
            FAIL("surface index " << index << " is not below " << g_layers.size());
        }
        std::lock_guard lock(g_layersMutex);
        if (g_layers[index] == nullptr)
        {
            const metal4::AutoreleasePool pool;
            g_layers[index] = CA::MetalLayer::layer()->retain();
            // A layer outside a window has no drawable size; each test surface starts with one, as a Null surface has.
            g_layers[index]->setDrawableSize(CGSize{ 256, 256 });
        }
        return g_layers[index];
    }

    //==================================================================================================================
    bool runsOnGpu()
    {
        return true;
    }
} // namespace rojoRHI::test
