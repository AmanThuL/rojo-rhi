//----------------------------------------------------------------------------------------------------------------------
/// @file NullShaderLibrary.h
/// @brief Declares the Null backend's shader library and pipeline cache blob formats.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace rojoRHI {
    /// The first line of a Null shader library; a blob that starts with anything else was not built for this backend.
    inline constexpr std::string_view kNullLibraryMagic = "rojorhi.null.shaderlibrary";

    /// The first line of a Null pipeline cache blob.
    inline constexpr std::string_view kNullCacheMagic = "rojorhi.null.pipelinecache";

    /// The Null pipeline cache format stamp, the second line's value; another stamp is another RojoRHI version.
    inline constexpr uint32_t kNullCacheVersion = 1;

    /// Parses a Null shader library. The blob is text, one line per fact: the magic line `rojorhi.null.shaderlibrary`, the
    /// line `abi <version>`, then one line `entry <name> <stage>` per entry point, where `<stage>` is vertex, fragment, task,
    /// mesh or compute. Another magic is `ErrorCategory::DriverFailure` (not built for this backend), as is a malformed line;
    /// another ABI version is `ErrorCategory::ShaderAbiMismatch`.
    Result<std::vector<core::ShaderEntryRecord>> parseNullShaderLibrary(std::span<const std::byte> blob);

    /// Serializes a pipeline cache blob: the magic line, the line `version <kNullCacheVersion>` and the line
    /// `pipelines <count>`, the number of pipelines the cache covers.
    std::vector<std::byte> serializeNullPipelineCache(uint64_t pipelines);

    /// Returns the pipeline count of a blob `serializeNullPipelineCache` wrote, or nothing when `blob` is of any other origin,
    /// version or shape.
    std::optional<uint64_t> parseNullPipelineCache(std::span<const std::byte> blob);
} // namespace rojoRHI
