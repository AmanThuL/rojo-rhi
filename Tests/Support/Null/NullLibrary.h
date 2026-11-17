//----------------------------------------------------------------------------------------------------------------------
/// @file NullLibrary.h
/// @brief Declares the helper that builds Null shader library blobs for the tests.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/Pipelines.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::test {
    struct LibraryEntry
    {
        std::string name;
        std::string stage; // vertex, fragment, task, mesh or compute; any other word builds a malformed library.
    };

    // The magic line, `abi <abi>` and one `entry` line per entry. A test passes another `magic` or `abi` to build a library
    // the Null backend refuses.
    std::vector<std::byte> nullLibrary(
        const std::vector<LibraryEntry>& entries, uint32_t abi = core::kShaderAbiVersion, std::string_view magic = "rojorhi.null.shaderlibrary"
    );
} // namespace rojoRHI::test
