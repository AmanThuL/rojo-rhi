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
    // At most six disjoint boxes.
    std::vector<TexelBox> subtract(const TexelBox& from, const TexelBox& cut);

    // Newly mapped tiles of one sparse texture subresource not discarded since: the first event that reaches one must be a
    // texture barrier from `Layout::Undefined` (VU-rojo-sparse-0014). The marks belong to the texture, whichever queue mapped
    // or uses the tiles.
    class TileMarks
    {
      public:
        void mark(const std::optional<TexelBox>& box);

        // A box leaves a mark of the whole subresource, which only a mip tail makes and only a change of the tail or a discard
        // clears.
        void clear(const std::optional<TexelBox>& box);

        bool reaches(const std::optional<TexelBox>& box) const;

        size_t count() const;

      private:
        bool m_whole = false;
        std::vector<TexelBox> m_boxes; // Disjoint after a clear; a later box contains none before it.
    };

    struct ByteMark
    {
        QueueType queue = QueueType::Graphics;
        uint64_t position = 0;
        // Per queue type, each distinct destination of the global barriers replayed there and ordered after the mapping.
        std::array<std::vector<Scope>, 3> reaches;

        bool operator==(const ByteMark&) const = default;
    };

    // A newly mapped range's first use must follow the mapping through a global barrier on its queue whose destination covers
    // it (VU-rojo-sparse-0014).
    class ByteMarks
    {
      public:
        // A remap forgets the barriers since the last mapping.
        void mark(uint64_t begin, uint64_t end, QueueType queue, uint64_t position);

        void clear(uint64_t begin, uint64_t end);

        // `ordered` is the submission's clock joined with the host clock.
        void reach(QueueType queue, const Clock& ordered, Scope destination);

        // Returns the first mark over the bytes that no global barrier on `queue` orders the use after; otherwise clears the
        // marks the use followed.
        std::optional<ByteMark> firstUse(uint64_t begin, uint64_t end, QueueType queue, Scope use);

        size_t count() const;

      private:
        OffsetMap<std::optional<ByteMark>> m_ranges;
    };
} // namespace rojoRHI::core
