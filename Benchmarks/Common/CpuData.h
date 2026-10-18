//----------------------------------------------------------------------------------------------------------------------
/// @file CpuData.h
/// @brief Declares the fixed bytes the CPU operation workloads upload and copy, the same in both arms and the oracles.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rojoRHI::bench {
    // C5 uploads and C7 copies this many bytes per repetition.
    inline constexpr uint32_t kCopyBytes = 256;

    // Byte i holds (7i + 3) mod 256: no two neighbours equal, so a shifted or partial copy shows.
    inline constexpr std::array<std::byte, kCopyBytes> copyPattern()
    {
        std::array<std::byte, kCopyBytes> bytes{};
        for (uint32_t index = 0; index < kCopyBytes; ++index)
        {
            bytes[index] = static_cast<std::byte>((7 * index + 3) & 0xFF);
        }
        return bytes;
    }
} // namespace rojoRHI::bench
