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
    inline constexpr std::string_view kLibraryMagic = "rojorhi.d3d12.shaderlibrary";

    struct LibraryEntry
    {
        std::string name;
        core::ShaderStage stage;
        std::array<uint32_t, 3> size = {}; // The threadgroup size; zero for vertex and fragment entries.
        std::span<const std::byte> dxil;   // A view into the parsed blob.
    };

    struct LibraryHeader
    {
        uint32_t abi = 0;
        std::vector<LibraryEntry> entries; // In file order.
    };

    // Another backend's blob, a bare DXIL container or a malformed header is `DriverFailure`; another ABI stamp is
    // `ShaderAbiMismatch`, reported before any entry line is read. The bytes after the `dxil` line are never read as
    // text, and each entry's slice is checked to start `DXBC` and nothing more.
    Result<LibraryHeader> parseShaderLibrary(std::span<const std::byte> blob);

    enum class MeshTopology : uint8_t
    {
        Unknown, // The container declares none this reader recognises.
        Line,
        Triangle,
    };

    // Read from the runtime information of the `PSV0` part: the shader kind at byte 24 must be mesh (13) and the
    // topology at byte 27 line (1) or triangle (2). A container without that part, too short, out of bounds, or of
    // another stage is `Unknown`.
    MeshTopology meshOutputTopology(std::span<const std::byte> dxil);

    // One copy of the DXIL region.
    struct LibraryPayload
    {
        std::vector<std::byte> dxil;                   // In entry order.
        std::vector<std::pair<size_t, size_t>> ranges; // `ranges[i]` is the offset and size of `record.entries[i]` in `dxil`.

        std::span<const std::byte> bytecode(size_t entry) const;
    };

    // `context` is unused.
    void retireShaderLibrary(void* context, core::ShaderLibraryRecord& record);
} // namespace rojoRHI::d3d12
