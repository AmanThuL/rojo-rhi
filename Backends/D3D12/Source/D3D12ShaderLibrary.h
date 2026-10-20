//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ShaderLibrary.h
/// @brief Declares the RojoRHI shader library container the D3D12 backend reads and the payload it keeps per library.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI::d3d12 {
    /// The first line of a RojoRHI shader library built for the D3D12 backend.
    inline constexpr std::string_view kLibraryMagic = "rojorhi.d3d12.shaderlibrary";

    /// One entry line of a library header, with the DXIL container the library holds for it.
    struct LibraryEntry
    {
        std::string name;                  ///< The entry point's name.
        core::ShaderStage stage;           ///< The stage it is compiled for.
        std::array<uint32_t, 3> size = {}; ///< The threadgroup size; zero for vertex and fragment.
        std::span<const std::byte> dxil;   ///< The entry's DXIL container; a view into the parsed blob.
    };

    /// What a library declares: its ABI stamp and its entries.
    struct LibraryHeader
    {
        uint32_t abi = 0;                  ///< The ABI version the library is stamped with.
        std::vector<LibraryEntry> entries; ///< The entries, in file order.
    };

    /// Parses `blob`. Another backend's blob, a bare DXIL container or a malformed header is `DriverFailure`; another ABI
    /// stamp is `ShaderAbiMismatch`, reported before any entry line is read. Pure C++: the bytes after the `dxil` line are
    /// never read as text, and each entry's slice is checked to start `DXBC` and nothing more.
    Result<LibraryHeader> parseShaderLibrary(std::span<const std::byte> blob);

    /// The output topology a mesh entry declares.
    enum class MeshTopology : uint8_t
    {
        Unknown,  ///< The container declares none that this reader recognises.
        Line,     ///< `outputtopology("line")`.
        Triangle, ///< `outputtopology("triangle")`.
    };

    /// Returns the output topology the mesh entry compiled into the DXIL container `dxil` declares, read from the runtime
    /// information of its pipeline state validation part (`PSV0`): the shader kind at byte 24 must be mesh (13) and the
    /// topology at byte 27 line (1) or triangle (2). A container without that part, too short, out of bounds, or of another
    /// stage is `Unknown`. Pure C++.
    MeshTopology meshOutputTopology(std::span<const std::byte> dxil);

    /// What a D3D12 shader library keeps in `ShaderLibraryRecord::native`: one copy of the DXIL region.
    struct LibraryPayload
    {
        std::vector<std::byte> dxil;                   ///< The DXIL containers of every entry, in entry order.
        std::vector<std::pair<size_t, size_t>> ranges; ///< `ranges[i]` is the offset and size of `record.entries[i]` in `dxil`.

        /// Returns the DXIL container of `record.entries[entry]`.
        std::span<const std::byte> bytecode(size_t entry) const;
    };

    /// Deletes the payload of a retiring library. `context` is unused; the hook signature carries it.
    void retireShaderLibrary(void* context, core::ShaderLibraryRecord& record);
} // namespace rojoRHI::d3d12
