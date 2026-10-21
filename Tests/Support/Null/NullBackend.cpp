//----------------------------------------------------------------------------------------------------------------------
/// @file NullBackend.cpp
/// @brief Implements the backend-dependent test support for the Null backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Backend.h"
#include "Support/Null/NullLibrary.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

namespace rojoRHI::test {
    namespace {
        /// Stand-ins for native surfaces: the Null backend never dereferences them.
        std::array<int, 8> g_surfaces = {};
    } // namespace

    //==================================================================================================================
    std::vector<std::byte> standardLibrary()
    {
        return libraryStampedWith(1);
    }

    //==================================================================================================================
    std::vector<std::byte> libraryStampedWith(uint32_t abi)
    {
        return nullLibrary({ { "vsMain", "vertex" }, { "fsMain", "fragment" }, { "msMain", "mesh" }, { "tsMain", "task" }, { "csMain", "compute" } }, abi);
    }

    //==================================================================================================================
    std::vector<std::byte> foreignLibrary()
    {
        const std::string_view text = "rojorhi.metal4.shaderlibrary\nabi 1\nentry vsMain vertex 0 0 0\nmetallib 0\n";
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        return { bytes, bytes + text.size() };
    }

    //==================================================================================================================
    // Returns no bytes and marks the running case failed: the Null backend packages no conformance shader, and its
    // conformance cases skip before they read one.
    std::vector<std::byte> shaderLibraryFile(std::string_view name)
    {
        FAIL_CHECK("the Null backend has no shader library file for " << name);
        return {};
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
    bool layoutsAreNative()
    {
        return false;
    }
} // namespace rojoRHI::test
