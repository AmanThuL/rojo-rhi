//----------------------------------------------------------------------------------------------------------------------
/// @file ByteSegments.h
/// @brief Declares the ordered disjoint byte segments of a buffer the validation tracker keeps hazard state for.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/Tracker/Hazards.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rojoRHI::core {
    struct ByteSegment
    {
        uint64_t begin = 0;
        uint64_t end = 0;
        UnitHazards hazards;
    };

    // Ordered, disjoint segments covering the whole buffer. A use or barrier splits them at its bounds, so it changes exactly
    // the bytes it names; `merge` joins neighbours left alike.
    class ByteSegments
    {
      public:
        explicit ByteSegments(uint64_t size);

        // The span is valid until the next call of a non-const member.
        std::span<ByteSegment> isolate(uint64_t begin, uint64_t end);

        // Joins neighbours from the segment before `begin` to the one after `end` whose states are `mergeable` below `floor`;
        // the merged segment keeps the state of its first part.
        void merge(uint64_t begin, uint64_t end, const std::array<uint64_t, 3>& floor);

        std::span<ByteSegment> segments();

        std::span<const ByteSegment> segments() const;

      private:
        // An `offset` equal to the size returns the number of segments.
        size_t splitAt(uint64_t offset);

        std::vector<ByteSegment> m_segments;
    };
} // namespace rojoRHI::core
