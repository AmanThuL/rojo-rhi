//----------------------------------------------------------------------------------------------------------------------
/// @file ByteSegments.cpp
/// @brief Implements the ordered disjoint byte segments of a buffer the validation tracker keeps hazard state for.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Tracker/ByteSegments.h"

#include "Base/Assert.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace rojoRHI::core {
    //==================================================================================================================
    ByteSegments::ByteSegments(uint64_t size) : m_segments{ ByteSegment{ .begin = 0, .end = size } }
    {
    }

    //==================================================================================================================
    std::span<ByteSegment> ByteSegments::isolate(uint64_t begin, uint64_t end)
    {
        ROJORHI_INTERNAL_ASSERT(begin < end && end <= m_segments.back().end, "a byte range outside its buffer was isolated");
        const size_t first = splitAt(begin);
        const size_t last = splitAt(end);
        return std::span(m_segments).subspan(first, last - first);
    }

    //==================================================================================================================
    void ByteSegments::merge(uint64_t begin, uint64_t end, const std::array<uint64_t, 3>& floor)
    {
        // The segments holding `begin` and `end - 1`, widened by one neighbour on each side.
        const auto holding = [&](uint64_t offset) {
            return static_cast<size_t>(std::distance(m_segments.begin(), std::ranges::upper_bound(m_segments, offset, {}, &ByteSegment::end)));
        };
        size_t first = holding(begin);
        size_t last = std::min(holding(end - 1) + 1, m_segments.size() - 1);
        first = first == 0 ? 0 : first - 1;
        size_t kept = first;
        for (size_t next = first + 1; next <= last; ++next)
        {
            if (mergeable(m_segments[kept].hazards, m_segments[next].hazards, floor))
            {
                m_segments[kept].end = m_segments[next].end;
                continue;
            }
            ++kept;
            if (kept != next)
            {
                m_segments[kept] = std::move(m_segments[next]);
            }
        }
        m_segments.erase(m_segments.begin() + static_cast<std::ptrdiff_t>(kept) + 1, m_segments.begin() + static_cast<std::ptrdiff_t>(last) + 1);
    }

    //==================================================================================================================
    std::span<ByteSegment> ByteSegments::segments()
    {
        return m_segments;
    }

    //==================================================================================================================
    std::span<const ByteSegment> ByteSegments::segments() const
    {
        return m_segments;
    }

    //==================================================================================================================
    size_t ByteSegments::splitAt(uint64_t offset)
    {
        // The first segment that ends after `offset` holds it.
        const auto holder = std::ranges::upper_bound(m_segments, offset, {}, &ByteSegment::end);
        if (holder == m_segments.end())
        {
            return m_segments.size();
        }
        const size_t index = static_cast<size_t>(std::distance(m_segments.begin(), holder));
        if (holder->begin == offset)
        {
            return index;
        }
        ByteSegment tail = *holder;
        tail.begin = offset;
        m_segments[index].end = offset;
        m_segments.insert(m_segments.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(tail));
        return index + 1;
    }
} // namespace rojoRHI::core
