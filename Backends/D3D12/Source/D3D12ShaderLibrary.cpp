//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ShaderLibrary.cpp
/// @brief Implements the parsing of the RojoRHI shader library container and the retirement of a library's payload.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12ShaderLibrary.h"

#include "Core/Pipelines.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace rojoRHI::d3d12 {
    namespace {
        //==============================================================================================================
        /// Builds the error of a malformed library.
        std::unexpected<Error> malformed(size_t line, std::string_view why)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("malformed D3D12 shader library: line {}: {}", line, why) });
        }

        //==============================================================================================================
        /// Reads a canonical unsigned decimal: digits only, no leading zero except for 0 itself.
        std::optional<uint64_t> canonicalNumber(std::string_view text)
        {
            if (text.empty() || (text.size() > 1 && text.front() == '0'))
            {
                return std::nullopt;
            }
            uint64_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                return std::nullopt;
            }
            return value;
        }

        //==============================================================================================================
        /// Whether `name` is `[A-Za-z_][A-Za-z0-9_]*`.
        bool validName(std::string_view name)
        {
            const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
            const auto digit = [](char c) { return c >= '0' && c <= '9'; };
            return !name.empty() && letter(name.front()) && std::ranges::all_of(name, [&](char c) { return letter(c) || digit(c); });
        }

        //==============================================================================================================
        /// Maps a stage word to its stage.
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
        /// Splits `line` at single spaces; empty tokens are kept.
        std::vector<std::string_view> splitSpaces(std::string_view line)
        {
            std::vector<std::string_view> tokens;
            for (size_t first = 0;;)
            {
                const size_t space = line.find(' ', first);
                tokens.push_back(line.substr(first, space == std::string_view::npos ? std::string_view::npos : space - first));
                if (space == std::string_view::npos)
                {
                    return tokens;
                }
                first = space + 1;
            }
        }

        /// A blob read one line at a time; nothing past the line last asked for is looked at.
        struct LineReader
        {
            std::span<const std::byte> blob; ///< The whole blob.
            size_t offset = 0;               ///< The first byte not yet returned.
            size_t line = 0;                 ///< The 1-based number of the line returned last.
        };

        //==============================================================================================================
        /// Returns the next line of `reader` without its newline, or nothing at the end of the blob.
        std::optional<std::string_view> nextLine(LineReader& reader)
        {
            if (reader.offset >= reader.blob.size())
            {
                return std::nullopt;
            }
            const auto* begin = reinterpret_cast<const char*>(reader.blob.data()) + reader.offset;
            const size_t left = reader.blob.size() - reader.offset;
            const void* newline = std::memchr(begin, '\n', left);
            const size_t length = newline == nullptr ? left : static_cast<size_t>(static_cast<const char*>(newline) - begin);
            reader.offset += std::min(left, length + 1);
            ++reader.line;
            return std::string_view(begin, length);
        }

        /// An entry line as read: the entry and the byte count of its DXIL.
        struct EntryLine
        {
            LibraryEntry entry; ///< The entry, without its DXIL slice.
            uint64_t bytes = 0; ///< The `<bytes>` token.
        };

        //==============================================================================================================
        /// Reads an `entry <name> <stage> <x> <y> <z> <bytes>` line, or returns why it is a breach.
        std::expected<EntryLine, std::string> readEntry(std::string_view line)
        {
            const std::vector<std::string_view> tokens = splitSpaces(line);
            if (tokens.size() != 7 || tokens[0] != "entry")
            {
                return std::unexpected(std::format("is not `entry <name> <stage> <x> <y> <z> <bytes>` ({} tokens)", tokens.size()));
            }
            if (!validName(tokens[1]))
            {
                return std::unexpected(std::format("has the entry name \"{}\", not [A-Za-z_][A-Za-z0-9_]*", tokens[1]));
            }
            const std::optional<core::ShaderStage> stage = stageOf(tokens[2]);
            if (!stage)
            {
                return std::unexpected(std::format("has the unknown stage \"{}\"", tokens[2]));
            }
            EntryLine result{ LibraryEntry{ std::string(tokens[1]), *stage, {}, {} }, 0 };
            LibraryEntry& entry = result.entry;
            for (size_t axis = 0; axis < 3; ++axis)
            {
                const std::optional<uint64_t> value = canonicalNumber(tokens[3 + axis]);
                if (!value || *value > UINT32_MAX)
                {
                    return std::unexpected(std::format("has the size \"{}\", not a canonical number", tokens[3 + axis]));
                }
                entry.size[axis] = static_cast<uint32_t>(*value);
            }
            const bool grouped = *stage == core::ShaderStage::Compute || *stage == core::ShaderStage::Task || *stage == core::ShaderStage::Mesh;
            const bool valid = grouped ? std::ranges::all_of(entry.size, [](uint32_t v) { return v >= 1; })
                                       : std::ranges::all_of(entry.size, [](uint32_t v) { return v == 0; });
            if (!valid)
            {
                return std::unexpected(
                    std::format(
                        "gives \"{}\" the size {} {} {}, which {} stage needs {}",
                        entry.name,
                        entry.size[0],
                        entry.size[1],
                        entry.size[2],
                        tokens[2],
                        grouped ? "at least 1 on every axis" : "to be 0 0 0"
                    )
                );
            }
            const std::optional<uint64_t> bytes = canonicalNumber(tokens[6]);
            if (!bytes)
            {
                return std::unexpected(std::format("has the byte count \"{}\", not a canonical number", tokens[6]));
            }
            result.bytes = *bytes;
            return result;
        }

        //==============================================================================================================
        /// Whether `bytes` begins with the DXIL container magic `DXBC`.
        bool startsDxbc(std::span<const std::byte> bytes)
        {
            return bytes.size() >= 4 && std::memcmp(bytes.data(), "DXBC", 4) == 0;
        }
    } // namespace

    //==================================================================================================================
    Result<LibraryHeader> parseShaderLibrary(std::span<const std::byte> blob)
    {
        LineReader reader{ blob };
        const std::optional<std::string_view> magic = nextLine(reader);
        if (!magic || *magic != kLibraryMagic)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, "the shader library was not built for the D3D12 backend" });
        }
        const std::optional<std::string_view> abiLine = nextLine(reader);
        const std::optional<uint64_t> abi = abiLine && abiLine->starts_with("abi ") ? canonicalNumber(abiLine->substr(4)) : std::nullopt;
        if (!abi)
        {
            return malformed(2, "is not `abi <version>`");
        }
        if (*abi != core::kShaderAbiVersion)
        {
            return std::unexpected(
                Error{ ErrorCategory::ShaderAbiMismatch,
                       std::format("the shader library has ABI version {}, this build needs {}", *abi, core::kShaderAbiVersion) }
            );
        }
        LibraryHeader header;
        header.abi = static_cast<uint32_t>(*abi);
        std::vector<uint64_t> bytes;
        std::vector<size_t> lines;
        for (;;)
        {
            const std::optional<std::string_view> line = nextLine(reader);
            if (!line)
            {
                return malformed(reader.line + 1, "is missing: the library has no `dxil <bytes>` line");
            }
            if (*line == "dxil" || line->starts_with("dxil "))
            {
                if (header.entries.empty())
                {
                    return malformed(reader.line, "is the `dxil` line, but no entry line precedes it");
                }
                const std::optional<uint64_t> total = line->size() > 5 ? canonicalNumber(line->substr(5)) : std::nullopt;
                if (!total)
                {
                    return malformed(reader.line, "is not `dxil <bytes>`");
                }
                uint64_t sum = 0;
                bool overflow = false;
                for (uint64_t entryBytes : bytes)
                {
                    overflow = overflow || entryBytes > UINT64_MAX - sum;
                    sum += entryBytes;
                }
                if (overflow || sum != *total)
                {
                    return malformed(reader.line, std::format("declares {} dxil bytes, but the entry lines add up to {}", *total, sum));
                }
                const std::span<const std::byte> rest = blob.subspan(reader.offset);
                if (*total != rest.size())
                {
                    return malformed(reader.line, std::format("declares {} dxil bytes, but {} follow", *total, rest.size()));
                }
                size_t offset = 0;
                for (size_t i = 0; i < header.entries.size(); ++i)
                {
                    header.entries[i].dxil = rest.subspan(offset, static_cast<size_t>(bytes[i]));
                    if (!startsDxbc(header.entries[i].dxil))
                    {
                        return malformed(lines[i], std::format("gives \"{}\" bytes that do not start with DXBC", header.entries[i].name));
                    }
                    offset += static_cast<size_t>(bytes[i]);
                }
                return header;
            }
            std::expected<EntryLine, std::string> entry = readEntry(*line);
            if (!entry)
            {
                return malformed(reader.line, entry.error());
            }
            if (std::ranges::any_of(header.entries, [&](const LibraryEntry& e) { return e.name == entry->entry.name; }))
            {
                return malformed(reader.line, std::format("repeats the entry name \"{}\"", entry->entry.name));
            }
            bytes.push_back(entry->bytes);
            lines.push_back(reader.line);
            header.entries.push_back(std::move(entry->entry));
        }
    }

    //==================================================================================================================
    std::span<const std::byte> LibraryPayload::bytecode(size_t entry) const
    {
        const auto [offset, size] = ranges[entry];
        return std::span<const std::byte>(dxil).subspan(offset, size);
    }

    //==================================================================================================================
    void retireShaderLibrary([[maybe_unused]] void* context, core::ShaderLibraryRecord& record)
    {
        delete static_cast<LibraryPayload*>(record.native);
        record.native = nullptr;
    }
} // namespace rojoRHI::d3d12
