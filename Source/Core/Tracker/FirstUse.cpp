//----------------------------------------------------------------------------------------------------------------------
/// @file FirstUse.cpp
/// @brief Implements the marks the validation tracker keeps of newly mapped sparse tiles until the first use that follows them.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Tracker/FirstUse.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        /// Returns the box of texels from (`x0`, `y0`, `z0`) up to, not including, (`x1`, `y1`, `z1`).
        TexelBox spanning(uint64_t x0, uint64_t y0, uint64_t z0, uint64_t x1, uint64_t y1, uint64_t z1)
        {
            return TexelBox{ .offset = { static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(z0) },
                             .extent = { static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0), static_cast<uint32_t>(z1 - z0) } };
        }
    } // namespace

    //==================================================================================================================
    std::vector<TexelBox> subtract(const TexelBox& from, const TexelBox& cut)
    {
        if (!overlaps(from, cut))
        {
            return { from };
        }
        const uint64_t fromX0 = from.offset.x, fromX1 = fromX0 + from.extent.width;
        const uint64_t fromY0 = from.offset.y, fromY1 = fromY0 + from.extent.height;
        const uint64_t fromZ0 = from.offset.z, fromZ1 = fromZ0 + from.extent.depth;
        // The texels both boxes hold.
        const uint64_t x0 = std::max<uint64_t>(fromX0, cut.offset.x), x1 = std::min<uint64_t>(fromX1, uint64_t{ cut.offset.x } + cut.extent.width);
        const uint64_t y0 = std::max<uint64_t>(fromY0, cut.offset.y), y1 = std::min<uint64_t>(fromY1, uint64_t{ cut.offset.y } + cut.extent.height);
        const uint64_t z0 = std::max<uint64_t>(fromZ0, cut.offset.z), z1 = std::min<uint64_t>(fromZ1, uint64_t{ cut.offset.z } + cut.extent.depth);
        // Slabs on either side along x, then within the shared x run along y, then within the shared x and y runs along z.
        std::vector<TexelBox> rest;
        if (fromX0 < x0)
        {
            rest.push_back(spanning(fromX0, fromY0, fromZ0, x0, fromY1, fromZ1));
        }
        if (x1 < fromX1)
        {
            rest.push_back(spanning(x1, fromY0, fromZ0, fromX1, fromY1, fromZ1));
        }
        if (fromY0 < y0)
        {
            rest.push_back(spanning(x0, fromY0, fromZ0, x1, y0, fromZ1));
        }
        if (y1 < fromY1)
        {
            rest.push_back(spanning(x0, y1, fromZ0, x1, fromY1, fromZ1));
        }
        if (fromZ0 < z0)
        {
            rest.push_back(spanning(x0, y0, fromZ0, x1, y1, z0));
        }
        if (z1 < fromZ1)
        {
            rest.push_back(spanning(x0, y0, z1, x1, y1, fromZ1));
        }
        return rest;
    }

    //==================================================================================================================
    void TileMarks::mark(const std::optional<TexelBox>& box)
    {
        if (!box.has_value())
        {
            m_whole = true;
            m_boxes.clear();
            return;
        }
        if (m_whole)
        {
            return;
        }
        std::erase_if(m_boxes, [&](const TexelBox& marked) { return contains(*box, marked); });
        m_boxes.push_back(*box);
    }

    //==================================================================================================================
    void TileMarks::clear(const std::optional<TexelBox>& box)
    {
        if (!box.has_value())
        {
            m_whole = false;
            m_boxes.clear();
            return;
        }
        std::vector<TexelBox> kept;
        for (const TexelBox& marked : m_boxes)
        {
            const std::vector<TexelBox> rest = subtract(marked, *box);
            kept.insert(kept.end(), rest.begin(), rest.end());
        }
        m_boxes = std::move(kept);
    }

    //==================================================================================================================
    bool TileMarks::reaches(const std::optional<TexelBox>& box) const
    {
        if (m_whole)
        {
            return true;
        }
        if (!box.has_value())
        {
            return !m_boxes.empty();
        }
        return std::ranges::any_of(m_boxes, [&](const TexelBox& marked) { return overlaps(*box, marked); });
    }

    //==================================================================================================================
    size_t TileMarks::count() const
    {
        return (m_whole ? 1 : 0) + m_boxes.size();
    }

    //==================================================================================================================
    void ByteMarks::mark(uint64_t begin, uint64_t end, QueueType queue, uint64_t position)
    {
        for (OffsetMap<std::optional<ByteMark>>::Range& range : m_ranges.isolate(begin, end))
        {
            range.state = ByteMark{ .queue = queue, .position = position };
        }
        m_ranges.merge(begin, end);
    }

    //==================================================================================================================
    void ByteMarks::clear(uint64_t begin, uint64_t end)
    {
        for (OffsetMap<std::optional<ByteMark>>::Range& range : m_ranges.isolate(begin, end))
        {
            range.state.reset();
        }
        m_ranges.merge(begin, end);
    }

    //==================================================================================================================
    void ByteMarks::reach(QueueType queue, const Clock& ordered, Scope destination)
    {
        bool reached = false;
        for (OffsetMap<std::optional<ByteMark>>::Range& range : m_ranges.ranges())
        {
            std::optional<ByteMark>& mark = range.state;
            // The barrier follows a mapping on its own queue in queue order, and one on another queue only through a fence or
            // the CPU's observation.
            if (!mark.has_value() || (mark->queue != queue && !covers(ordered, mark->queue, mark->position)))
            {
                continue;
            }
            std::vector<Scope>& reaches = mark->reaches[static_cast<size_t>(queue)];
            if (std::ranges::find(reaches, destination) == reaches.end())
            {
                reaches.push_back(destination);
                reached = true;
            }
        }
        if (reached)
        {
            m_ranges.merge(0, std::numeric_limits<uint64_t>::max());
        }
    }

    //==================================================================================================================
    std::optional<ByteMark> ByteMarks::firstUse(uint64_t begin, uint64_t end, QueueType queue, Scope use)
    {
        if (begin >= end)
        {
            return std::nullopt;
        }
        bool marked = false;
        for (const OffsetMap<std::optional<ByteMark>>::Range& range : m_ranges.ranges())
        {
            const std::optional<ByteMark>& mark = range.state;
            if (!mark.has_value() || range.end <= begin || end <= range.begin)
            {
                continue;
            }
            // The use sees the new memory as an aliasing barrier's destination sees it: every stage and access it makes.
            const std::vector<Scope>& reaches = mark->reaches[static_cast<size_t>(queue)];
            if (std::ranges::none_of(reaches, [&](Scope destination) { return destinationCovers(destination, use, true); }))
            {
                return mark;
            }
            marked = true;
        }
        if (marked)
        {
            clear(begin, end);
        }
        return std::nullopt;
    }

    //==================================================================================================================
    size_t ByteMarks::count() const
    {
        return static_cast<size_t>(std::ranges::count_if(m_ranges.ranges(), [](const OffsetMap<std::optional<ByteMark>>::Range& range) {
            return range.state.has_value();
        }));
    }
} // namespace rojoRHI::core
