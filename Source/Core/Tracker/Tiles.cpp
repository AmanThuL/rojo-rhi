//----------------------------------------------------------------------------------------------------------------------
/// @file Tiles.cpp
/// @brief Implements the events the validation tracker keeps per queue for the tiles of sparse resources.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Tracker/Tiles.h"

#include <algorithm>
#include <limits>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        /// Returns true when the runs of `aSize` texels from `aBegin` and of `bSize` from `bBegin` share a texel.
        bool overlapsOnAxis(uint32_t aBegin, uint32_t aSize, uint32_t bBegin, uint32_t bSize)
        {
            return uint64_t{ aBegin } < uint64_t{ bBegin } + bSize && uint64_t{ bBegin } < uint64_t{ aBegin } + aSize;
        }

        //==============================================================================================================
        /// Returns true when the run of `innerSize` texels from `innerBegin` lies in the run of `outerSize` from `outerBegin`.
        bool containsOnAxis(uint32_t outerBegin, uint32_t outerSize, uint32_t innerBegin, uint32_t innerSize)
        {
            return outerBegin <= innerBegin && uint64_t{ innerBegin } + innerSize <= uint64_t{ outerBegin } + outerSize;
        }
    } // namespace

    //==================================================================================================================
    bool overlaps(const TexelBox& a, const TexelBox& b)
    {
        return overlapsOnAxis(a.offset.x, a.extent.width, b.offset.x, b.extent.width) &&
               overlapsOnAxis(a.offset.y, a.extent.height, b.offset.y, b.extent.height) &&
               overlapsOnAxis(a.offset.z, a.extent.depth, b.offset.z, b.extent.depth);
    }

    //==================================================================================================================
    bool contains(const TexelBox& outer, const TexelBox& inner)
    {
        return containsOnAxis(outer.offset.x, outer.extent.width, inner.offset.x, inner.extent.width) &&
               containsOnAxis(outer.offset.y, outer.extent.height, inner.offset.y, inner.extent.height) &&
               containsOnAxis(outer.offset.z, outer.extent.depth, inner.offset.z, inner.extent.depth);
    }

    //==================================================================================================================
    void TileEvents::note(const std::optional<TexelBox>& box, uint64_t position, uint64_t floor)
    {
        if (!box.has_value())
        {
            // Every part is older, so the whole event is the latest of each of its texels.
            m_whole = position;
            m_parts.clear();
            return;
        }
        std::erase_if(m_parts, [&](const std::pair<TexelBox, uint64_t>& part) { return part.second <= floor || contains(*box, part.first); });
        if (m_whole <= floor)
        {
            m_whole = 0;
        }
        m_parts.emplace_back(*box, position);
    }

    //==================================================================================================================
    uint64_t TileEvents::latest(const std::optional<TexelBox>& box) const
    {
        uint64_t found = m_whole;
        for (const auto& [part, position] : m_parts)
        {
            if (!box.has_value() || overlaps(*box, part))
            {
                found = std::max(found, position);
            }
        }
        return found;
    }

    //==================================================================================================================
    size_t TileEvents::parts() const
    {
        return m_parts.size();
    }

    //==================================================================================================================
    void ByteEvents::note(uint64_t begin, uint64_t end, uint64_t position, uint64_t floor)
    {
        for (OffsetMap<uint64_t>::Range& range : m_ranges.ranges())
        {
            if (range.state <= floor)
            {
                range.state = 0;
            }
        }
        for (OffsetMap<uint64_t>::Range& range : m_ranges.isolate(begin, end))
        {
            range.state = position;
        }
        m_ranges.merge(0, std::numeric_limits<uint64_t>::max());
    }

    //==================================================================================================================
    uint64_t ByteEvents::latest(uint64_t begin, uint64_t end) const
    {
        uint64_t found = 0;
        for (const OffsetMap<uint64_t>::Range& range : m_ranges.ranges())
        {
            if (range.begin < end && begin < range.end)
            {
                found = std::max(found, range.state);
            }
        }
        return found;
    }

    //==================================================================================================================
    size_t ByteEvents::ranges() const
    {
        return m_ranges.ranges().size();
    }

    //==================================================================================================================
    std::optional<TileRace> findRace(const std::array<TileEvents, 3>& events, const std::optional<TexelBox>& box, QueueType queue, const Clock& ordered)
    {
        for (size_t index = 0; index < events.size(); ++index)
        {
            const QueueType other = static_cast<QueueType>(index);
            const uint64_t position = events[index].latest(box);
            if (other != queue && position != 0 && !covers(ordered, other, position))
            {
                return TileRace{ .queue = other, .position = position };
            }
        }
        return std::nullopt;
    }

    //==================================================================================================================
    std::optional<TileRace> findRace(const std::array<ByteEvents, 3>& events, uint64_t begin, uint64_t end, QueueType queue, const Clock& ordered)
    {
        for (size_t index = 0; index < events.size(); ++index)
        {
            const QueueType other = static_cast<QueueType>(index);
            const uint64_t position = events[index].latest(begin, end);
            if (other != queue && position != 0 && !covers(ordered, other, position))
            {
                return TileRace{ .queue = other, .position = position };
            }
        }
        return std::nullopt;
    }
} // namespace rojoRHI::core
