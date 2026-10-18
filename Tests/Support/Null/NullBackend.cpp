//----------------------------------------------------------------------------------------------------------------------
/// @file NullBackend.cpp
/// @brief Implements the backend-dependent test support for the Null backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Backend.h"
#include "Support/NullLibrary.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace rojoRHI::test {
    namespace {
        /// Stand-ins for native surfaces: the Null backend never dereferences them.
        std::array<int, 8> g_surfaces = {};
    } // namespace

    //==================================================================================================================
    std::vector<std::byte> standardLibrary()
    {
        return nullLibrary({ { "vsMain", "vertex" }, { "fsMain", "fragment" }, { "msMain", "mesh" }, { "tsMain", "task" }, { "csMain", "compute" } });
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
} // namespace rojoRHI::test
