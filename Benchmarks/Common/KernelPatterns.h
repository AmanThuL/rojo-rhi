//----------------------------------------------------------------------------------------------------------------------
/// @file KernelPatterns.h
/// @brief Declares the fixed data and index formulas of the GPU kernels G1 and G2, mirrored in every kernel and used by the oracles.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace rojoRHI::bench {
    // One dispatch of kKernelThreads threads in groups of kKernelGroup; each thread sums kKernelLoads values.
    inline constexpr uint32_t kKernelThreads = 1u << 20;
    inline constexpr uint32_t kKernelGroup = 64;
    inline constexpr uint32_t kKernelLoads = 16;
    // G1 reads kDataEntries uint4; G2 reads kTextureCount textures of kTextureSize squared texels.
    inline constexpr uint32_t kDataEntries = 1u << 16;
    inline constexpr uint32_t kTextureCount = 128;
    inline constexpr uint32_t kTextureSize = 16;

    // One G1 entry: four unsigned words, 16 bytes.
    using DataEntry = std::array<uint32_t, 4>;

    // The 32-bit hash every formula below mixes with; the shaders spell it out the same way.
    inline constexpr uint32_t mix(uint32_t value)
    {
        const uint32_t state = value * 747796405u + 2891336453u;
        const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        return (word >> 22u) ^ word;
    }

    // The key a thread draws its indices from: one value per SIMD group of 32 when uniform, a hash of the thread when not.
    inline constexpr uint32_t kernelKey(uint32_t thread, bool divergent)
    {
        return divergent ? mix(thread) : thread / 32u;
    }

    // The j-th of the 16 indices of a key, below `count`, a power of two.
    inline constexpr uint32_t kernelIndex(uint32_t key, uint32_t j, uint32_t count)
    {
        return mix(key * 16u + j) & (count - 1u);
    }

    // Entry i of G1's data.
    inline constexpr DataEntry dataEntry(uint32_t i)
    {
        return DataEntry{ i * 3u + 1u, i * 5u + 2u, i * 7u + 3u, i * 11u + 4u };
    }

    // The integer below 2^16 in texel (x, y) of texture t.
    inline constexpr uint32_t texelValue(uint32_t t, uint32_t x, uint32_t y)
    {
        return (t * 2053u + x * 131u + y * 1031u) & 0xFFFFu;
    }

    // The texel the j-th sample of a thread reads: column j, row 5j mod 16.
    inline constexpr uint32_t sampleX(uint32_t j)
    {
        return j & (kTextureSize - 1u);
    }
    inline constexpr uint32_t sampleY(uint32_t j)
    {
        return (j * 5u) & (kTextureSize - 1u);
    }

    // G1's output entry for a thread: the sum of its 16 entries, each word wrapping at 2^32.
    DataEntry expectedLoads(uint32_t thread, bool divergent);
    // G2's output for a thread: the sum of its 16 samples, exact in float below 2^24.
    float expectedSamples(uint32_t thread, bool divergent);

    // G1's data as bytes, kDataEntries entries in index order.
    std::vector<DataEntry> dataTable();
    // Texture t's texels as floats, row-major.
    std::vector<float> textureTexels(uint32_t t);
} // namespace rojoRHI::bench
