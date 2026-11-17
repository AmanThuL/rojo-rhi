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
    inline constexpr std::string_view kNullLibraryMagic = "rojorhi.null.shaderlibrary";

    inline constexpr std::string_view kNullCacheMagic = "rojorhi.null.pipelinecache";

    // Another stamp is another RojoRHI version.
    inline constexpr uint32_t kNullCacheVersion = 1;

    // A Null shader library is text: the magic line, `abi <version>`, then one `entry <name> <stage>` per entry point, where
    // `<stage>` is vertex, fragment, task, mesh or compute. Another magic or a malformed line is `DriverFailure`; another ABI
    // version is `ShaderAbiMismatch`.
    Result<std::vector<core::ShaderEntryRecord>> parseNullShaderLibrary(std::span<const std::byte> blob);

    // A pipeline cache blob is three lines: the magic line, `version <kNullCacheVersion>` and `pipelines <count>`.
    std::vector<std::byte> serializeNullPipelineCache(uint64_t pipelines);

    std::optional<uint64_t> parseNullPipelineCache(std::span<const std::byte> blob);
} // namespace rojoRHI
