//----------------------------------------------------------------------------------------------------------------------
/// @file NullShaderLibrary.cpp
/// @brief Implements the parsing and serialization of the Null backend's blob formats.
//----------------------------------------------------------------------------------------------------------------------

#include "NullShaderLibrary.h"

#include "Core/Pipelines.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Splits `text` into its lines, dropping one empty line at the end.
        std::vector<std::string_view> splitLines(std::string_view text)
        {
            std::vector<std::string_view> lines;
            while (!text.empty())
            {
                const size_t end = text.find('\n');
                lines.push_back(text.substr(0, end));
                text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            }
            return lines;
        }

        //==============================================================================================================
        /// Returns the text of `blob`.
        std::string_view textOf(std::span<const std::byte> blob)
        {
            return { reinterpret_cast<const char*>(blob.data()), blob.size() };
        }

        //==============================================================================================================
        /// Reads the number in `text` after `prefix`, or nothing when the line has another prefix or is not a plain number.
        std::optional<uint64_t> numberAfter(std::string_view text, std::string_view prefix)
        {
            if (!text.starts_with(prefix))
            {
                return std::nullopt;
            }
            const std::string_view digits = text.substr(prefix.size());
            uint64_t value = 0;
            const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
            if (error != std::errc{} || end != digits.data() + digits.size())
            {
                return std::nullopt;
            }
            return value;
        }

        //==============================================================================================================
        /// Maps a stage word of a library line to its stage.
        std::optional<core::ShaderStage> stageOf(std::string_view word)
        {
            for (core::ShaderStage stage :
                 { core::ShaderStage::Vertex, core::ShaderStage::Fragment, core::ShaderStage::Task, core::ShaderStage::Mesh, core::ShaderStage::Compute })
            {
                if (core::stageName(stage) == word)
                {
                    return stage;
                }
            }
            return std::nullopt;
        }

        //==============================================================================================================
        /// Builds the error of a malformed library.
        std::unexpected<Error> malformed(std::string_view why)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("malformed Null shader library: {}", why) });
        }
    } // namespace

    //==================================================================================================================
    Result<std::vector<core::ShaderEntryRecord>> parseNullShaderLibrary(std::span<const std::byte> blob)
    {
        const std::vector<std::string_view> lines = splitLines(textOf(blob));
        if (lines.empty() || lines[0] != kNullLibraryMagic)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, "the shader library was not built for the Null backend" });
        }
        const std::optional<uint64_t> abi = lines.size() > 1 ? numberAfter(lines[1], "abi ") : std::nullopt;
        if (!abi)
        {
            return malformed("the second line is not `abi <version>`");
        }
        if (*abi != core::kShaderAbiVersion)
        {
            return std::unexpected(
                Error{ ErrorCategory::ShaderAbiMismatch,
                       std::format("the shader library has ABI version {}, this build needs {}", *abi, core::kShaderAbiVersion) }
            );
        }
        std::vector<core::ShaderEntryRecord> entries;
        for (size_t i = 2; i < lines.size(); ++i)
        {
            const std::string_view line = lines[i];
            const size_t space = line.rfind(' ');
            const std::optional<core::ShaderStage> stage = space == std::string_view::npos ? std::nullopt : stageOf(line.substr(space + 1));
            if (!line.starts_with("entry ") || !stage || space <= 6)
            {
                return malformed(std::format("line {} is not `entry <name> <stage>`", i + 1));
            }
            if (std::ranges::any_of(entries, [&](const core::ShaderEntryRecord& e) { return e.name == line.substr(6, space - 6); }))
            {
                return malformed(std::format("line {} repeats the entry name \"{}\"", i + 1, line.substr(6, space - 6)));
            }
            entries.push_back({ std::string(line.substr(6, space - 6)), *stage });
        }
        return entries;
    }

    //==================================================================================================================
    std::vector<std::byte> serializeNullPipelineCache(uint64_t pipelines)
    {
        const std::string text = std::format("{}\nversion {}\npipelines {}\n", kNullCacheMagic, kNullCacheVersion, pipelines);
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        return { bytes, bytes + text.size() };
    }

    //==================================================================================================================
    std::optional<uint64_t> parseNullPipelineCache(std::span<const std::byte> blob)
    {
        const std::vector<std::string_view> lines = splitLines(textOf(blob));
        if (lines.size() != 3 || lines[0] != kNullCacheMagic || numberAfter(lines[1], "version ") != kNullCacheVersion)
        {
            return std::nullopt;
        }
        return numberAfter(lines[2], "pipelines ");
    }
} // namespace rojoRHI
