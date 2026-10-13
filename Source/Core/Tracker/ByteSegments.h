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
    /// One byte range of a buffer whose bytes share one hazard state.
    struct ByteSegment
    {
        uint64_t begin = 0;  ///< The first byte.
        uint64_t end = 0;    ///< One past the last byte.
        UnitHazards hazards; ///< The hazard state of every byte of the segment.
    };

    /// The bytes of one buffer as ordered, disjoint segments that together cover the whole buffer. A use or barrier splits the
    /// segments where its range starts and ends, so it changes exactly the bytes it names; `merge` joins neighbours left alike.
    class ByteSegments
    {
      public:
        /// Covers `size` bytes with one segment of no hazard state.
        explicit ByteSegments(uint64_t size);

        /// Splits the segments so that `begin` and `end` are boundaries and returns the segments of `begin` to `end`. `begin` is
        /// below `end`, which is at most the size. The span is valid until the next call of a non-const member.
        std::span<ByteSegment> isolate(uint64_t begin, uint64_t end);

        /// Joins neighbouring segments around the bytes `begin` to `end`, from the segment before them to the one after, whose
        /// hazard states are alike with every position below `floor` of its queue type (`mergeable`). The merged segment keeps
        /// the state of its first part.
        void merge(uint64_t begin, uint64_t end, const std::array<uint64_t, 3>& floor);

        /// Returns every segment in byte order.
        std::span<ByteSegment> segments();

        /// Returns every segment in byte order.
        std::span<const ByteSegment> segments() const;

      private:
        /// Splits the segment that holds `offset` so that a segment starts at it, and returns that segment's index; `offset`
        /// equal to the size returns the number of segments.
        size_t splitAt(uint64_t offset);

        std::vector<ByteSegment> m_segments; ///< The segments in byte order.
    };
} // namespace rojoRHI::core
