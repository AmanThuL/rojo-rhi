//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4ShaderLibrary.h
/// @brief Declares the RojoRHI shader library container the Metal 4 backend reads and the payload it keeps per library.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::metal4 {
    inline constexpr std::string_view kLibraryMagic = "rojorhi.metal4.shaderlibrary";

    struct LibraryEntry
    {
        std::string name;
        core::ShaderStage stage;
        std::array<uint32_t, 3> size = {}; // Zero for vertex and fragment.
    };

    struct LibraryHeader
    {
        uint32_t abi = 0;
        std::vector<LibraryEntry> entries;
        std::span<const std::byte> metallib; // A view into the parsed blob.
    };

    // Another ABI stamp is `ShaderAbiMismatch`, reported before any entry line is read; any other bad blob is
    // `DriverFailure`. No Metal object is touched, and the bytes after the `metallib` line are never read as text.
    Result<LibraryHeader> parseShaderLibrary(std::span<const std::byte> blob);

    struct LibraryPayload
    {
        MTL::Library* library = nullptr;            // +1.
        std::vector<std::array<uint32_t, 3>> sizes; // `sizes[i]` is the threadgroup size of `record.entries[i]`.

        void release();
    };

    void retireShaderLibrary(void* context, core::ShaderLibraryRecord& record);
} // namespace rojoRHI::metal4
