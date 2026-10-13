//----------------------------------------------------------------------------------------------------------------------
/// @file OffsetMap.h
/// @brief Declares the ordered disjoint byte ranges, each with one state, that the validation tracker keeps over heaps.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Base/Assert.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace rojoRHI::core {
    /// One byte range of an `OffsetMap` whose bytes share one state.
    template<typename State> struct OffsetRange
    {
        uint64_t begin = 0; ///< The first byte.
        uint64_t end = 0;   ///< One past the last byte.
        State state = {};   ///< The state of every byte of the range.

        bool operator==(const OffsetRange&) const = default;
    };

    /// Every byte offset as ordered, disjoint ranges from zero to the largest offset, each holding one `State`, which starts
    /// value-initialised. A change splits the ranges where it starts and ends, so it changes exactly the bytes it names;
    /// `merge` joins neighbours left equal, so the ranges stay as few as the distinct states.
    template<typename State> class OffsetMap
    {
      public:
        using Range = OffsetRange<State>; ///< One range and its state.

        /// Covers every byte offset with one range of the initial state.
        OffsetMap() : m_ranges{ Range{ .begin = 0, .end = std::numeric_limits<uint64_t>::max() } }
        {
        }

        /// Splits the ranges so that `begin` and `end` are boundaries and returns the ranges of `begin` to `end`. `begin` is
        /// below `end`, which is below the largest offset. The span is valid until the next call of a non-const member.
        std::span<Range> isolate(uint64_t begin, uint64_t end)
        {
            ROJORHI_INTERNAL_ASSERT(begin < end, "an empty byte range was isolated");
            const size_t first = splitAt(begin);
            const size_t last = splitAt(end);
            return std::span(m_ranges).subspan(first, last - first);
        }

        /// Joins neighbouring ranges around the bytes `begin` to `end`, from the range before them to the one after, whose
        /// states are equal.
        void merge(uint64_t begin, uint64_t end)
        {
            size_t first = holding(begin);
            const size_t last = std::min(holding(end - 1) + 1, m_ranges.size() - 1);
            first = first == 0 ? 0 : first - 1;
            size_t kept = first;
            for (size_t next = first + 1; next <= last; ++next)
            {
                if (m_ranges[kept].state == m_ranges[next].state)
                {
                    m_ranges[kept].end = m_ranges[next].end;
                    continue;
                }
                ++kept;
                if (kept != next)
                {
                    m_ranges[kept] = std::move(m_ranges[next]);
                }
            }
            m_ranges.erase(m_ranges.begin() + static_cast<std::ptrdiff_t>(kept) + 1, m_ranges.begin() + static_cast<std::ptrdiff_t>(last) + 1);
        }

        /// Returns every range in byte order.
        std::span<Range> ranges()
        {
            return m_ranges;
        }

        /// Returns every range in byte order.
        std::span<const Range> ranges() const
        {
            return m_ranges;
        }

        bool operator==(const OffsetMap&) const = default;

      private:
        /// Returns the index of the range that holds `offset`.
        size_t holding(uint64_t offset) const
        {
            return static_cast<size_t>(std::distance(m_ranges.begin(), std::ranges::upper_bound(m_ranges, offset, {}, &Range::end)));
        }

        /// Splits the range that holds `offset` so that a range starts at it, and returns that range's index.
        size_t splitAt(uint64_t offset)
        {
            const size_t index = holding(offset);
            ROJORHI_INTERNAL_ASSERT(index < m_ranges.size(), "a byte range reaches the largest offset");
            if (m_ranges[index].begin == offset)
            {
                return index;
            }
            Range tail = m_ranges[index];
            tail.begin = offset;
            m_ranges[index].end = offset;
            m_ranges.insert(m_ranges.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(tail));
            return index + 1;
        }

        std::vector<Range> m_ranges; ///< The ranges in byte order.
    };
} // namespace rojoRHI::core
