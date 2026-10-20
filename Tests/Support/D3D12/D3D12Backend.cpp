//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Backend.cpp
/// @brief Implements the backend-dependent test support for the D3D12 backend, which creates no device, so no case reaches it.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace rojoRHI::test {
    //==================================================================================================================
    // Fails the running case, which ends it, as every entry here but runsOnGpu does: the backend creates no device.
    std::vector<std::byte> standardLibrary()
    {
        FAIL("the D3D12 test support has no standard library: the backend creates no device");
    }

    //==================================================================================================================
    std::vector<std::byte> libraryStampedWith(uint32_t)
    {
        FAIL("the D3D12 test support has no stamped library: the backend creates no device");
    }

    //==================================================================================================================
    std::vector<std::byte> foreignLibrary()
    {
        FAIL("the D3D12 test support has no foreign library: the backend creates no device");
    }

    //==================================================================================================================
    std::vector<std::byte> shaderLibraryFile(std::string_view)
    {
        FAIL("the D3D12 test support has no shader library file: the backend creates no device");
    }

    //==================================================================================================================
    void* surface(uint32_t)
    {
        FAIL("the D3D12 test support has no surface: the backend creates no device");
    }

    //==================================================================================================================
    bool runsOnGpu()
    {
        return true;
    }
} // namespace rojoRHI::test
