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
    /// The first line of a RojoRHI shader library built for the Metal 4 backend.
    inline constexpr std::string_view kLibraryMagic = "rojorhi.metal4.shaderlibrary";

    /// One entry line of a library header.
    struct LibraryEntry
    {
        std::string name;                  ///< The entry point's name.
        core::ShaderStage stage;           ///< The stage it is compiled for.
        std::array<uint32_t, 3> size = {}; ///< The threadgroup size; zero for vertex and fragment.
    };

    /// What the header of a library declares, and the metallib bytes after it.
    struct LibraryHeader
    {
        uint32_t abi = 0;                    ///< The ABI version the library is stamped with.
        std::vector<LibraryEntry> entries;   ///< The entry lines, in file order.
        std::span<const std::byte> metallib; ///< The bytes after the `metallib` line; a view into the parsed blob.
    };

    /// Parses the header of `blob`. Another backend's blob, a bare metallib or a malformed header is `DriverFailure`; another
    /// ABI stamp is `ShaderAbiMismatch`, reported before any entry line is read. Pure C++: no Metal object is touched, and the
    /// bytes after the `metallib` line are never read as text.
    Result<LibraryHeader> parseShaderLibrary(std::span<const std::byte> blob);

    /// What a Metal 4 shader library keeps in `ShaderLibraryRecord::native`.
    struct LibraryPayload
    {
        MTL::Library* library = nullptr;            ///< The library (+1).
        std::vector<std::array<uint32_t, 3>> sizes; ///< `sizes[i]` is the threadgroup size of `record.entries[i]`.

        /// Releases the library; a null library is skipped.
        void release();
    };

    /// Releases the payload of a retiring library and deletes it. `context` is the `DeviceImpl`.
    void retireShaderLibrary(void* context, core::ShaderLibraryRecord& record);
} // namespace rojoRHI::metal4
