//----------------------------------------------------------------------------------------------------------------------
/// @file FirstUse.h
/// @brief Declares the marks the validation tracker keeps of newly mapped sparse tiles until the first use that follows them.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Tracker/Clocks.h"
#include "Core/Tracker/Hazards.h"
#include "Core/Tracker/OffsetMap.h"
#include "Core/Tracker/Tiles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rojoRHI::core {
    /// Returns the texels of `from` outside `cut`, as at most six disjoint boxes: `from` itself when the two share no texel,
    /// none when `cut` contains it.
    std::vector<TexelBox> subtract(const TexelBox& from, const TexelBox& cut);

    /// The tiles of one subresource of a sparse texture that `Queue::mapTiles` newly mapped onto heap memory and that no texture
    /// barrier from `Layout::Undefined` has discarded since: the first event that reaches one must be that discard
    /// (VU-rojo-sparse-0014). The marks belong to the texture, whichever queue mapped the tiles or uses them.
    class TileMarks
    {
      public:
        /// Marks the texels `box`, or the whole subresource when `box` is empty, as newly mapped: a mapping or a remap.
        void mark(const std::optional<TexelBox>& box);

        /// Clears the marks of the texels `box`, or every mark when `box` is empty: an unmap, or a discard of the subresource.
        /// A box leaves a mark of the whole subresource, which only a mip tail makes and only a change of the tail or a discard clears.
        void clear(const std::optional<TexelBox>& box);

        /// Returns true when an event of the texels `box` reaches a mark: the boxes intersect, or the whole subresource is
        /// marked. An event with no box, of the whole subresource, reaches any mark.
        bool reaches(const std::optional<TexelBox>& box) const;

        /// Returns the number of marks kept, a mark of the whole subresource counting as one.
        size_t count() const;

      private:
        bool m_whole = false;          ///< The whole subresource is marked.
        std::vector<TexelBox> m_boxes; ///< The marked boxes, disjoint after a clear, a later one containing none before it.
    };

    /// A byte range of a sparse buffer that `Queue::mapTiles` newly mapped onto heap memory: the mapping's queue and position
    /// there, and per queue type the destinations of the global barriers replayed there since and ordered after the mapping.
    struct ByteMark
    {
        QueueType queue = QueueType::Graphics;     ///< The queue of the mapping.
        uint64_t position = 0;                     ///< The mapping's position there.
        std::array<std::vector<Scope>, 3> reaches; ///< Per queue type, the destination of each such global barrier, once.

        bool operator==(const ByteMark&) const = default;
    };

    /// The newly mapped byte ranges of one sparse buffer whose first use is still to come: that use must be ordered after the
    /// mapping by a global barrier on its queue whose destination covers it (VU-rojo-sparse-0014).
    class ByteMarks
    {
      public:
        /// Marks the bytes `begin` to `end` as mapped at `position` on `queue`; a remap forgets the barriers since the last.
        void mark(uint64_t begin, uint64_t end, QueueType queue, uint64_t position);

        /// Clears the marks of the bytes `begin` to `end`: an unmap leaves no first use to order.
        void clear(uint64_t begin, uint64_t end);

        /// Notes a global barrier to `destination` replayed on `queue`, where `ordered`, the clock of its submission joined
        /// with the host clock, orders earlier work: every mark it follows, one mapped on `queue` or whose mapping `ordered`
        /// covers, gains its destination there.
        void reach(QueueType queue, const Clock& ordered, Scope destination);

        /// Judges `use`, a use on `queue` of the bytes `begin` to `end`. Returns the first mark over them that no global barrier
        /// on `queue` orders the use after, its destination covering the use's stages and accesses; otherwise clears every mark
        /// over them, which the use followed, and returns nothing.
        std::optional<ByteMark> firstUse(uint64_t begin, uint64_t end, QueueType queue, Scope use);

        /// Returns the number of marked byte ranges kept.
        size_t count() const;

      private:
        OffsetMap<std::optional<ByteMark>> m_ranges; ///< The mark of each byte range; none for bytes not newly mapped.
    };
} // namespace rojoRHI::core
