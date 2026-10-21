//----------------------------------------------------------------------------------------------------------------------
/// @file Outputs.cpp
/// @brief Implements output hashing (CommonCrypto on macOS, CNG on Windows), dump files and byte comparison.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/Outputs.h"

#if defined(_WIN32)
#include <windows.h>

#include <bcrypt.h>
#else
#include <CommonCrypto/CommonDigest.h>
#endif

#include <array>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <system_error>

namespace rojoRHI::bench {
    //==================================================================================================================
    std::expected<std::string, std::string> sha256Hex(std::span<const std::byte> bytes)
    {
#if defined(_WIN32)
        std::array<unsigned char, 32> digest{};
        // BCryptHash takes a 32-bit length, an output being far below 4 GiB, and a non-const input it only reads.
        const NTSTATUS status = BCryptHash(
            BCRYPT_SHA256_ALG_HANDLE,
            nullptr,
            0,
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
            static_cast<ULONG>(bytes.size()),
            digest.data(),
            static_cast<ULONG>(digest.size())
        );
        if (!BCRYPT_SUCCESS(status))
        {
            return std::unexpected(std::format("sha256 failed with NTSTATUS 0x{:08x}", static_cast<uint32_t>(status)));
        }
#else
        std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
        // CC_SHA256 takes a 32-bit length; an output is far below 4 GiB.
        CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
#endif
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
            std::ofstream file(path, std::ios::binary);
            if (!file)
            {
                return std::unexpected(std::format("cannot open {} for writing", path.string()));
            }
            file.write(reinterpret_cast<const char*>(output.bytes.data()), static_cast<std::streamsize>(output.bytes.size()));
            file.close();
            if (!file)
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
