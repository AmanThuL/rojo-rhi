//----------------------------------------------------------------------------------------------------------------------
/// @file NullLibrary.h
/// @brief Declares the helper that builds Null shader library blobs for the tests.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::test {
    /// One entry point of a Null library under test.
    struct LibraryEntry
    {
        std::string name;  ///< The entry point's name.
        std::string stage; ///< vertex, fragment, task, mesh or compute; any other word builds a malformed library.
    };

    /// Builds the text blob of a Null shader library: the magic line, `abi <abi>` and one `entry` line per entry. A test passes
    /// another `magic` or `abi` to build a library the Null backend refuses.
    std::vector<std::byte> nullLibrary(const std::vector<LibraryEntry>& entries, uint32_t abi = 1, std::string_view magic = "rojorhi.null.shaderlibrary");
} // namespace rojoRHI::test
