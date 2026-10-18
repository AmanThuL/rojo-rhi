//----------------------------------------------------------------------------------------------------------------------
/// @file Outputs.cpp
/// @brief Implements output hashing through CommonCrypto, dump files and byte comparison.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/Outputs.h"

#include <CommonCrypto/CommonDigest.h>

#include <array>
#include <cstdio>
#include <format>
#include <system_error>

namespace rojoRHI::bench {
    //==================================================================================================================
    std::string sha256Hex(std::span<const std::byte> bytes)
    {
        std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
        // CC_SHA256 takes a 32-bit length; an output is far below 4 GiB.
        CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
        std::string text;
        text.reserve(2 * digest.size());
        for (const unsigned char value : digest)
        {
            text += std::format("{:02x}", value);
        }
        return text;
    }

    //==================================================================================================================
    std::expected<void, std::string> dumpOutputs(const std::filesystem::path& directory, std::string_view id, Arm arm, std::span<const Output> outputs)
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            return std::unexpected(std::format("cannot create the dump directory {}: {}", directory.string(), error.message()));
        }
        for (const Output& output : outputs)
        {
            const std::filesystem::path path = directory / std::format("{}.{}.{}.bin", id, armName(arm), output.name);
            std::FILE* file = std::fopen(path.c_str(), "wb");
            if (file == nullptr)
            {
                return std::unexpected(std::format("cannot open {} for writing", path.string()));
            }
            const size_t written = output.bytes.empty() ? 0 : std::fwrite(output.bytes.data(), 1, output.bytes.size(), file);
            const bool closed = std::fclose(file) == 0;
            if (written != output.bytes.size() || !closed)
            {
                return std::unexpected(std::format("cannot write the {} bytes of {}", output.bytes.size(), path.string()));
            }
        }
        return {};
    }

    //==================================================================================================================
    bool sameOutputs(std::span<const Output> first, std::span<const Output> second)
    {
        if (first.size() != second.size())
        {
            return false;
        }
        for (size_t index = 0; index < first.size(); ++index)
        {
            if (first[index].name != second[index].name || first[index].bytes != second[index].bytes)
            {
                return false;
            }
        }
        return true;
    }
} // namespace rojoRHI::bench
