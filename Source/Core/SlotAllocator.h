//----------------------------------------------------------------------------------------------------------------------
/// @file SlotAllocator.h
/// @brief Declares the allocator of heap slots with deferred reuse, one instance per heap: the view heap and the sampler heap.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Base/Assert.h"

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
        Released, ///< Released at a frame that has not retired; not handed out again yet.
        Reserved  ///< Set aside by `reserveLeading` for the backend; never handed out.
    };

    /// Hands out the indices of one global heap. A slot released at frame F returns to the free list only when `retire` is
    /// called with a frame at or past F, so no slot is reused while unretired work may read it. Every slot keeps the `owner`
    /// word its allocator stored, which the caller uses to find what the slot describes, and a `native` payload, the backend's
    /// object for the slot or null. A payload leaves its slot when the slot is released or a write replaces it, waits for the
    /// frame of that release or write, and is handed to the caller exactly once: by `retire` when that frame retires, or by
    /// `drain`. A backend may set aside a leading block of slots for its own descriptors with `reserveLeading`; the
    /// `capacity` the allocating calls take then counts the slots after that block. All operations take one mutex.
    class SlotAllocator
    {
      public:
        /// Sets aside slots [0, `count`) for the backend, on an allocator that has handed out nothing: they are never handed
        /// out, are Reserved rather than Assigned, so `assignedCount` does not count them, and cannot be released, written or
        /// retired. A later `capacity` counts the slots from `count` on, so the highest index handed out stays below
        /// `count + capacity`. `drain` ends the reservation with everything else.
        void reserveLeading(uint32_t count);

        /// Takes a free slot, preferring a retired one over a fresh one, and records `owner` and `native` in it. `capacity` is
        /// the number of slots the heap has after the reserved block; returns nothing when every slot is assigned or waiting to
        /// retire.
        std::optional<uint32_t> allocate(uint32_t capacity, uint64_t owner, void* native = nullptr);

        /// Takes `count` contiguous free slots, retired or never handed out, and records `owner` and a null payload in each;
        /// they belong to one range until `releaseRange`. Returns the first index, or nothing when no run of `count` slots is
        /// free; `count` is not zero.
        std::optional<uint32_t> allocateRange(uint32_t capacity, uint32_t count, uint64_t owner);

        /// Returns true when the range is currently reserved as one run with this `base` and `count`. It cannot tell a
        /// stale value from an identical later reservation of the same slots.
        bool holdsRange(uint32_t base, uint32_t count);

        /// Replaces the owner word and payload of slot `index`, which must be a slot of an assigned range, and returns the
        /// previous owner; returns nothing, changing nothing, for any other slot. The previous payload, when not null, waits
        /// for the frame `frameOf()` returns, which runs under the allocator's lock and must not call the allocator.
        template<typename FrameSupplier> std::optional<uint64_t> replace(uint32_t index, uint64_t owner, void* native, FrameSupplier&& frameOf)
        {
            std::lock_guard lock(m_mutex);
            if (index >= m_slots.size() || m_slots[index].state != SlotState::Assigned || !m_slots[index].inRange)
            {
                return std::nullopt;
            }
            Slot& slot = m_slots[index];
            if (slot.native != nullptr)
            {
                m_pending.push_back(Pending{ frameOf(), index, 0, PendingKind::Payload, slot.native, {} });
            }
            slot.native = native;
            return std::exchange(slot.owner, owner);
        }

        /// Releases the assigned range that starts at `base` and holds `count` slots, as `release` does a slot: all its
        /// slots, and the payloads they hold, wait for the same frame. Returns the owner words of its slots in order, or
        /// nothing, releasing nothing, when no such range is assigned.
        template<typename FrameSupplier> std::optional<std::vector<uint64_t>> releaseRange(uint32_t base, uint32_t count, FrameSupplier&& frameOf)
        {
            std::lock_guard lock(m_mutex);
            if (base >= m_slots.size() || m_slots[base].state != SlotState::Assigned || m_slots[base].rangeCount != count || count == 0)
            {
                return std::nullopt;
            }
            std::vector<uint64_t> owners;
            owners.reserve(count);
            Pending pending{ frameOf(), base, count, PendingKind::Range, nullptr, {} };
            for (uint32_t i = 0; i < count; ++i)
            {
                Slot& slot = m_slots[base + i];
                slot.state = SlotState::Released;
                owners.push_back(slot.owner);
                if (slot.native != nullptr)
                {
                    pending.natives.push_back(std::exchange(slot.native, nullptr));
                }
            }
            m_pending.push_back(std::move(pending));
            return owners;
        }

        /// Releases the assigned slot `index`: it, and the payload it holds, are handed out again only after `retire` reaches
        /// the frame `frameOf()` returns, which runs under the allocator's lock so a `retire` cannot slip between reading the
        /// frame and recording the release. `frameOf` must not call the allocator. Returns the slot's owner, or nothing,
        /// releasing nothing, when the slot is not assigned or belongs to a range.
        template<typename FrameSupplier> std::optional<uint64_t> release(uint32_t index, FrameSupplier&& frameOf)
        {
            std::lock_guard lock(m_mutex);
            if (index >= m_slots.size() || m_slots[index].state != SlotState::Assigned || m_slots[index].rangeCount != 0 || m_slots[index].inRange)
            {
                return std::nullopt;
            }
            Slot& slot = m_slots[index];
            slot.state = SlotState::Released;
            m_pending.push_back(Pending{ frameOf(), index, 1, PendingKind::Single, std::exchange(slot.native, nullptr), {} });
            return slot.owner;
        }

        /// Makes every slot released at a frame up to and including `upToFrame` available for reuse, and calls `onNative` with
        /// each non-null payload released or replaced at such a frame. `onNative` runs under the allocator's lock and must not
        /// call the allocator. Returns how many single slots (released by `release`, not as part of a range) that made
        /// available; payloads from ranges and from replaced writes are not counted.
        template<typename OnNative> uint64_t retire(uint64_t upToFrame, OnNative&& onNative)
        {
            std::lock_guard lock(m_mutex);
            uint64_t singles = 0;
            std::erase_if(m_pending, [&](const Pending& entry) {
                if (entry.frame > upToFrame)
                {
                    return false;
                }
                handOver(entry, onNative);
                for (uint32_t i = 0; i < entry.count; ++i)
                {
                    m_slots[entry.index + i] = Slot{};
                    m_free.push_back(entry.index + i);
                }
                singles += entry.kind == PendingKind::Single ? 1 : 0;
                return true;
            });
            return singles;
        }

        /// Retires as the other overload does, for a heap whose slots hold no payload; a payload reaching it is an internal
        /// error.
        uint64_t retire(uint64_t upToFrame);

        /// Calls `onNative` once with every non-null payload a slot holds or a release or write left waiting, then empties
        /// the allocator and ends any reservation, so every slot is Free and fresh slots start from zero again. `onNative` runs under the allocator's
        /// lock and must not call the allocator.
        template<typename OnNative> void drain(OnNative&& onNative)
        {
            std::lock_guard lock(m_mutex);
            for (const Slot& slot : m_slots)
            {
                // A released slot's payload moved to its pending entry, so no payload is both held and pending.
                ROJORHI_INTERNAL_ASSERT(slot.state == SlotState::Assigned || slot.native == nullptr, "a slot that is not assigned holds a payload");
                if (slot.native != nullptr)
                {
                    onNative(slot.native);
                }
            }
            for (const Pending& entry : m_pending)
            {
                handOver(entry, onNative);
            }
            m_slots.clear();
            m_free.clear();
            m_pending.clear();
            m_reserved = 0;
        }

        /// Returns the state of slot `index`; a slot never handed out is Free.
        SlotState state(uint32_t index);

        /// Returns the number of slots in state Assigned.
        size_t assignedCount();

        /// Returns true when no slot was handed out since the allocator was created or drained and nothing waits to retire; a
        /// reserved block alone leaves it empty.
        bool empty();

      private:
        struct Slot
        {
            SlotState state = SlotState::Free;
            uint64_t owner = 0;
            uint32_t rangeCount = 0; ///< Slots of the range this slot starts; zero for any other slot.
            bool inRange = false;    ///< True for every slot of a range, its first included.
            void* native = nullptr;  ///< The backend's object for the slot; null when it has none or it was released.
        };

        /// What a pending entry waits to give back.
        enum class PendingKind : uint8_t
        {
            Single,  ///< A slot released by `release`.
            Range,   ///< A range released by `releaseRange`.
            Payload, ///< A payload a write replaced; its slot stays assigned.
        };

        struct Pending
        {
            uint64_t frame;
            uint32_t index;
            uint32_t count; ///< Slots the entry returns to the free list; zero for a replaced payload.
            PendingKind kind;
            void* native;               ///< The payload of a Single or Payload entry; may be null.
            std::vector<void*> natives; ///< The non-null payloads of a Range entry's slots; empty for any other entry.
        };

        /// Calls `onNative` with each non-null payload `entry` holds.
        template<typename OnNative> static void handOver(const Pending& entry, OnNative& onNative)
        {
            if (entry.native != nullptr)
            {
                onNative(entry.native);
            }
            for (void* native : entry.natives)
            {
                onNative(native);
            }
        }

        std::mutex m_mutex;
        std::vector<Slot> m_slots;      ///< Slots handed out at least once; the index is the position.
        std::vector<uint32_t> m_free;   ///< Retired slots, reused before a fresh one.
        std::vector<Pending> m_pending; ///< Released slots, ranges and replaced payloads in release order.
        uint32_t m_reserved = 0;        ///< Leading slots set aside by `reserveLeading`.
    };
} // namespace rojoRHI::core
