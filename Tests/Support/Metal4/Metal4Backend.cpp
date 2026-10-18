//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Backend.cpp
/// @brief Implements the backend-dependent test support for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Backend.h"

#include "Metal4Common.h"

#include <catch2/catch_test_macros.hpp>

#include <Foundation/Foundation.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rojoRHI::test {
    namespace {
        /// The layers `surface` hands out, created on first use and retained for the process.
        std::array<CA::MetalLayer*, 8> g_layers = {};

        /// Guards `g_layers`, because cases may ask for a surface from several threads.
        std::mutex g_layersMutex;
    } // namespace

    //==================================================================================================================
    std::vector<std::byte> standardLibrary()
    {
        FAIL("the Metal 4 standard library is packaged with shader library support");
        return {};
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
        }
        return g_layers[index];
    }

    //==================================================================================================================
    bool runsOnGpu()
    {
        return true;
    }
} // namespace rojoRHI::test
