//----------------------------------------------------------------------------------------------------------------------
/// @file Tiles.h
/// @brief Declares the events the validation tracker keeps per queue for the tiles of sparse resources: uses and mapping changes.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Tracker/Clocks.h"
#include "Core/Tracker/OffsetMap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace rojoRHI::core {
    // A mapping region is a union of whole tiles, clipped at the level's edge, so a box overlaps it exactly when it touches
    // one of its tiles.
    struct TexelBox
    {
        Offset3D offset;
        Extent3D extent;
    };

    bool overlaps(const TexelBox& a, const TexelBox& b);

    bool contains(const TexelBox& outer, const TexelBox& inner);

    // Positions only grow on a queue, so the latest event a query overlaps is the one a clock must cover for every earlier one
    // to be covered too.
    class TileEvents
    {
      public:
        // Events at or below `floor`, which the CPU observed complete, order nothing more and are forgotten, as are parts a
        // later event contains.
        void note(const std::optional<TexelBox>& box, uint64_t position, uint64_t floor);

        uint64_t latest(const std::optional<TexelBox>& box) const;

        size_t parts() const;

      private:
        uint64_t m_whole = 0;
        std::vector<std::pair<TexelBox, uint64_t>> m_parts;
    };

    class ByteEvents
    {
      public:
        // Events at or below `floor`, which the CPU observed complete, order nothing more and are forgotten.
        void note(uint64_t begin, uint64_t end, uint64_t position, uint64_t floor);

        uint64_t latest(uint64_t begin, uint64_t end) const;

        size_t ranges() const;

      private:
        OffsetMap<uint64_t> m_ranges;
    };

    struct TileRace
    {
        QueueType queue = QueueType::Graphics;
        uint64_t position = 0;
    };

    std::optional<TileRace> findRace(const std::array<TileEvents, 3>& events, const std::optional<TexelBox>& box, QueueType queue, const Clock& ordered);

    std::optional<TileRace> findRace(const std::array<ByteEvents, 3>& events, uint64_t begin, uint64_t end, QueueType queue, const Clock& ordered);

    struct TileChange
    {
        Texture texture = {};
        SubresourceRange range;
        std::optional<TexelBox> box; // Empty for the whole of each subresource, as a mip tail changes.
        Buffer buffer = {};
        uint64_t offset = 0;
        uint64_t size = 0;
        bool unmap = false; // An unmap marks nothing.
    };
} // namespace rojoRHI::core
