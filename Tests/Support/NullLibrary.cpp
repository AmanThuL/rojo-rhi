//----------------------------------------------------------------------------------------------------------------------
/// @file NullLibrary.cpp
/// @brief Implements the helper that builds Null shader library blobs for the tests.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/NullLibrary.h"

#include <format>

namespace rojoRHI::test {
    //==================================================================================================================
    std::vector<std::byte> nullLibrary(const std::vector<LibraryEntry>& entries, uint32_t abi, std::string_view magic)
    {
        std::string text = std::format("{}\nabi {}\n", magic, abi);
        for (const LibraryEntry& entry : entries)
        {
            text += std::format("entry {} {}\n", entry.name, entry.stage);
        }
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        return { bytes, bytes + text.size() };
    }

    //==================================================================================================================
    std::vector<std::byte> standardLibrary()
    {
        return nullLibrary({ { "vsMain", "vertex" }, { "fsMain", "fragment" }, { "msMain", "mesh" }, { "tsMain", "task" }, { "csMain", "compute" } });
    }
} // namespace rojoRHI::test
