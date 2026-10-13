//----------------------------------------------------------------------------------------------------------------------
/// @file SlotAllocator.h
/// @brief Declares the allocator of heap slots with deferred reuse, one instance per heap: the view heap and the sampler heap.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace rojoRHI::core {
    /// What one slot of the heap holds.
    enum class SlotState : uint8_t
    {
        Free,     ///< Never handed out, or retired and waiting in the free list.
        Assigned, ///< Handed out by `allocate` and not released.
        Released  ///< Released at a frame that has not retired; not handed out again yet.
    };

    /// Hands out the indices of one global heap. A slot released at frame F returns to the free list only when `retire` is
    /// called with a frame at or past F, so no slot is reused while unretired work may read it. Every slot keeps the `owner`
    /// word its allocator stored, which the caller uses to find what the slot describes. All operations take one mutex.
    class SlotAllocator
    {
      public:
        /// Takes a free slot, preferring a retired one over a fresh one, and records `owner` in it. `capacity` is the number of
        /// slots the heap has; returns nothing when every slot is assigned or waiting to retire.
        std::optional<uint32_t> allocate(uint32_t capacity, uint64_t owner);

        /// Takes `count` contiguous free slots, retired or never handed out, and records `owner` in each; they belong to one
        /// range until `releaseRange`. Returns the first index, or nothing when no run of `count` slots is free; `count` is not
        /// zero.
        std::optional<uint32_t> allocateRange(uint32_t capacity, uint32_t count, uint64_t owner);

        /// Returns true when the range is currently reserved as one run with this `base` and `count`. It cannot tell a
        /// stale value from an identical later reservation of the same slots.
        bool holdsRange(uint32_t base, uint32_t count);

        /// Replaces the owner word of slot `index`, which must be a slot of an assigned range, and returns the previous one;
        /// returns nothing, changing nothing, for any other slot.
        std::optional<uint64_t> replaceOwner(uint32_t index, uint64_t owner);

        /// Releases the assigned range that starts at `base` and holds `count` slots, as `release` does a slot: all its
        /// slots wait for the same frame. Returns the owner words of its slots in order, or nothing, releasing nothing, when
        /// no such range is assigned.
        template<typename FrameSupplier> std::optional<std::vector<uint64_t>> releaseRange(uint32_t base, uint32_t count, FrameSupplier&& frameOf)
        {
            std::lock_guard lock(m_mutex);
            if (base >= m_slots.size() || m_slots[base].state != SlotState::Assigned || m_slots[base].rangeCount != count || count == 0)
            {
                return std::nullopt;
            }
            std::vector<uint64_t> owners;
            owners.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                m_slots[base + i].state = SlotState::Released;
                owners.push_back(m_slots[base + i].owner);
            }
            m_pending.push_back(Pending{ frameOf(), base, count });
            return owners;
        }

        /// Releases the assigned slot `index`: it is handed out again only after `retire` reaches the frame `frameOf()` returns,
        /// which runs under the allocator's lock so a `retire` cannot slip between reading the frame and recording the release.
        /// `frameOf` must not call the allocator. Returns the slot's owner, or nothing, releasing nothing, when the slot is not
        /// assigned or belongs to a range.
        template<typename FrameSupplier> std::optional<uint64_t> release(uint32_t index, FrameSupplier&& frameOf)
        {
            std::lock_guard lock(m_mutex);
            if (index >= m_slots.size() || m_slots[index].state != SlotState::Assigned || m_slots[index].rangeCount != 0 || m_slots[index].inRange)
            {
                return std::nullopt;
            }
            Slot& slot = m_slots[index];
            slot.state = SlotState::Released;
            m_pending.push_back(Pending{ frameOf(), index, 1 });
            return slot.owner;
        }

        /// Makes every slot released at a frame up to and including `upToFrame` available for reuse.
        void retire(uint64_t upToFrame);

        /// Returns the state of slot `index`; a slot never handed out is Free.
        SlotState state(uint32_t index);

        /// Returns the number of slots in state Assigned.
        size_t assignedCount();

      private:
        struct Slot
        {
            SlotState state = SlotState::Free;
            uint64_t owner = 0;
            uint32_t rangeCount = 0; ///< Slots of the range this slot starts; zero for any other slot.
            bool inRange = false;    ///< True for every slot of a range, its first included.
        };

        struct Pending
        {
            uint64_t frame;
            uint32_t index;
            uint32_t count;
        };

        std::mutex m_mutex;
        std::vector<Slot> m_slots;      ///< Slots handed out at least once; the index is the position.
        std::vector<uint32_t> m_free;   ///< Retired slots, reused before a fresh one.
        std::vector<Pending> m_pending; ///< Released slots and ranges in release order.
    };
} // namespace rojoRHI::core
