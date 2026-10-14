//----------------------------------------------------------------------------------------------------------------------
/// @file Tiles.h
/// @brief Declares the events the validation tracker keeps per queue for the tiles of sparse resources: uses and mapping changes.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Geometry.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/QueueType.h>
#include <rojoRHI/SubresourceRange.h>

#include "Core/Tracker/Clocks.h"
#include "Core/Tracker/OffsetMap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace rojoRHI::core {
    /// A box of texels in one subresource: the region of a copy, or the region a mapping change names. A mapping region is a
    /// union of whole tiles, clipped at the level's edge, so a box overlaps it exactly when it touches one of its tiles.
    struct TexelBox
    {
        Offset3D offset; ///< The first texel.
        Extent3D extent; ///< Size in texels.
    };

    /// Returns true when `a` and `b` share a texel.
    bool overlaps(const TexelBox& a, const TexelBox& b);

    /// Returns true when every texel of `inner` lies in `outer`.
    bool contains(const TexelBox& outer, const TexelBox& inner);

    /// The events on one queue of one subresource of a sparse texture: the position of the latest event of the whole
    /// subresource, and the box and position of each later event of part of it. Positions only grow on a queue, so the latest
    /// event a query overlaps is the one a clock must cover for every earlier one to be covered too.
    class TileEvents
    {
      public:
        /// Notes an event at `position` of `box`, or of the whole subresource when `box` is empty. Events at or below `floor`,
        /// which the CPU observed complete, order nothing more and are forgotten, as are parts a later event contains.
        void note(const std::optional<TexelBox>& box, uint64_t position, uint64_t floor);

        /// Returns the position of the latest event that overlaps `box`, or of any event when `box` is empty; zero for none.
        uint64_t latest(const std::optional<TexelBox>& box) const;

        /// Returns the number of events of part of the subresource kept.
        size_t parts() const;

      private:
        uint64_t m_whole = 0;                               ///< The latest event of the whole subresource; zero for none.
        std::vector<std::pair<TexelBox, uint64_t>> m_parts; ///< The later events of part of it, each with its position.
    };

    /// The events on one queue of the byte ranges of a sparse buffer: per range, the position of its latest event.
    class ByteEvents
    {
      public:
        /// Notes an event at `position` of the bytes `begin` to `end`. Events at or below `floor`, which the CPU observed
        /// complete, order nothing more and are forgotten.
        void note(uint64_t begin, uint64_t end, uint64_t position, uint64_t floor);

        /// Returns the position of the latest event of any of the bytes `begin` to `end`; zero for none.
        uint64_t latest(uint64_t begin, uint64_t end) const;

        /// Returns the number of byte ranges kept, those of no event included.
        size_t ranges() const;

      private:
        OffsetMap<uint64_t> m_ranges; ///< The position of the latest event of each byte range; zero for none.
    };

    /// The latest event on another queue than `queue` that `ordered`, the clock of work on `queue` joined with the host clock,
    /// does not cover: its queue and its position.
    struct TileRace
    {
        QueueType queue = QueueType::Graphics; ///< The queue of the event.
        uint64_t position = 0;                 ///< Its position there.
    };

    /// Returns the first event of `events`, one `TileEvents` per queue type, on a queue other than `queue` that overlaps `box`
    /// and that `ordered` does not cover, or nothing.
    std::optional<TileRace> findRace(const std::array<TileEvents, 3>& events, const std::optional<TexelBox>& box, QueueType queue, const Clock& ordered);

    /// Returns the first event of `events`, one `ByteEvents` per queue type, on a queue other than `queue` of any of the bytes
    /// `begin` to `end` that `ordered` does not cover, or nothing.
    std::optional<TileRace> findRace(const std::array<ByteEvents, 3>& events, uint64_t begin, uint64_t end, QueueType queue, const Clock& ordered);

    /// One change `Queue::mapTiles` makes, as the tracker follows it: for a texture, the subresources whose tiles change and
    /// the box of texels that changes in each, or the whole of each for a mip tail; for a buffer, a byte range.
    struct TileChange
    {
        Texture texture = {};        ///< The sparse texture; null for a buffer change.
        SubresourceRange range;      ///< The subresources of the texture that change.
        std::optional<TexelBox> box; ///< The texels that change in each; empty for the whole subresource.
        Buffer buffer = {};          ///< The sparse buffer; null for a texture change.
        uint64_t offset = 0;         ///< The first byte of the buffer range.
        uint64_t size = 0;           ///< Bytes of the buffer range.
    };
} // namespace rojoRHI::core
