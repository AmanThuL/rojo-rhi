//----------------------------------------------------------------------------------------------------------------------
/// @file CountedData.h
/// @brief Declares the records, counts and expected image of the counted-draw workloads, the same in both arms and the oracles.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rojoRHI::bench {
    // The target's edge in pixels; a draw's drawId is the pixel it fills, row by row.
    inline constexpr uint32_t kCountedTargetEdge = 256;
    inline constexpr uint32_t kCountedTargetBytes = kCountedTargetEdge * kCountedTargetEdge * sizeof(float);
    // C10's maxCount, and the count its count buffer holds.
    inline constexpr uint32_t kCountedMaxCount = 256;
    // The drawId of the one record of C10m1.
    inline constexpr uint32_t kSingleDrawId = 13;

    // Record i draws one point at pixel drawIdOf(i). Over a power-of-two maxCount the ids are a permutation of
    // [0, maxCount), since 7919 is odd; over any other maxCount they are distinct below the next power of two.
    inline constexpr uint32_t drawIdOf(uint32_t index, uint32_t maxCount)
    {
        const uint32_t range = std::bit_ceil(maxCount);
        return static_cast<uint32_t>((7919ull * index + 13) % range);
    }

    // maxCount records of DrawIndirectArgs, one instance of one vertex each. The firstInstance is non-zero and must
    // not show: the shader reads the drawId only.
    inline std::vector<std::byte> drawRecords(uint32_t maxCount)
    {
        std::vector<std::byte> bytes(uint64_t{ maxCount } * sizeof(DrawIndirectArgs));
        for (uint32_t index = 0; index < maxCount; ++index)
        {
            const DrawIndirectArgs record{ drawIdOf(index, maxCount), 1, 1, 0, 1000 + index };
            std::memcpy(bytes.data() + uint64_t{ index } * sizeof(record), &record, sizeof(record));
        }
        return bytes;
    }

    // maxCount records of DrawIndexedIndirectArgs over an index buffer whose first UInt16 index is 0.
    inline std::vector<std::byte> indexedRecords(uint32_t maxCount)
    {
        std::vector<std::byte> bytes(uint64_t{ maxCount } * sizeof(DrawIndexedIndirectArgs));
        for (uint32_t index = 0; index < maxCount; ++index)
        {
            const DrawIndexedIndirectArgs record{ drawIdOf(index, maxCount), 1, 1, 0, 0, 1000 + index };
            std::memcpy(bytes.data() + uint64_t{ index } * sizeof(record), &record, sizeof(record));
        }
        return bytes;
    }

    // The target after records [0, drawn) of a maxCount draw: drawId + 1 as a float at pixel drawId, 0 elsewhere.
    inline std::vector<std::byte> expectedTarget(uint32_t maxCount, uint32_t drawn)
    {
        std::vector<std::byte> bytes(kCountedTargetBytes);
        for (uint32_t index = 0; index < drawn; ++index)
        {
            const uint32_t drawId = drawIdOf(index, maxCount);
            const float value = static_cast<float>(drawId + 1);
            std::memcpy(bytes.data() + uint64_t{ drawId } * sizeof(float), &value, sizeof(value));
        }
        return bytes;
    }

    // The target after C10m1's one point.
    inline std::vector<std::byte> expectedSinglePoint()
    {
        std::vector<std::byte> bytes(kCountedTargetBytes);
        const float value = static_cast<float>(kSingleDrawId + 1);
        std::memcpy(bytes.data() + uint64_t{ kSingleDrawId } * sizeof(float), &value, sizeof(value));
        return bytes;
    }
} // namespace rojoRHI::bench
