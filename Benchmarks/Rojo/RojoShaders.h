//----------------------------------------------------------------------------------------------------------------------
/// @file RojoShaders.h
/// @brief Declares how the RojoRHI arm loads a shader library: a text blob on Null, the built metallib on Metal 4.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace rojoRHI::bench {
    // One entry point a workload's pipelines use.
    struct ShaderEntryName
    {
        std::string_view name;
        std::string_view stage; ///< "vertex", "fragment", "task", "mesh" or "compute".
    };

    // Loads library `name` with the label "bench.<name>". On Null the blob lists exactly `entries`; on Metal 4 it is
    // Shaders/<name>.metallib beside the executable, which holds them. Destroying the library is the caller's.
    std::expected<ShaderLibrary, std::string> loadLibrary(Device device, std::string_view name, std::span<const ShaderEntryName> entries);
} // namespace rojoRHI::bench
