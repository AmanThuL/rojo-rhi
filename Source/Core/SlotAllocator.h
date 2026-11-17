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
    enum class SlotState : uint8_t
    {
        Free, // Never handed out, or retired.
        Assigned,
        Released, // Waits for its release frame to retire.
        Reserved  // Set aside by `reserveLeading`; never handed out.
    };

    // Hands out the indices of one global heap. A slot released at frame F returns to the free list only when `retire`
    // reaches F, so no slot is reused while unretired work may read it. Each slot keeps its allocator's `owner` word
    // and a `native` payload, the backend's object or null. A payload leaves its slot on release or replacement, waits
    // for that frame, and is handed back exactly once: by `retire` or by `drain`. With `reserveLeading`, the `capacity`
    // the allocating calls take counts the slots after the reserved block. All operations take one mutex.
    class SlotAllocator
    {
      public:
        // Only on an allocator that has handed out nothing. Reserved slots are never handed out, released, written or
        // retired, and `assignedCount` skips them; the highest index handed out stays below `count + capacity`. `drain`
        // ends the reservation.
        void reserveLeading(uint32_t count);

        // Prefers a retired slot over a fresh one. Empty when every slot is assigned or waiting to retire.
        std::optional<uint32_t> allocate(uint32_t capacity, uint64_t owner, void* native = nullptr);

        // The slots form one range until `releaseRange`, each with a null payload; `count` is not zero.
        std::optional<uint32_t> allocateRange(uint32_t capacity, uint32_t count, uint64_t owner);

        // Cannot tell a stale value from an identical later reservation of the same slots.
        bool holdsRange(uint32_t base, uint32_t count);

        // Only for a slot of an assigned range; for any other slot it changes nothing and returns empty. The previous
        // payload waits for the frame `frameOf()` returns; `frameOf` runs under the allocator's lock and must not call
        // the allocator.
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

        // All its slots and their payloads wait for the same frame. Empty, releasing nothing, when no such range is
        // assigned.
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

        // `frameOf` runs under the allocator's lock, so a `retire` cannot slip between reading the frame and recording
        // the release; it must not call the allocator. Empty, releasing nothing, when the slot is not assigned or
        // belongs to a range.
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

        // `onNative` runs under the allocator's lock and must not call the allocator. Returns how many single slots,
        // not range slots, became free; payloads of ranges and replaced writes are not counted.
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

        // For a heap whose slots hold no payload; a payload reaching it is an internal error.
        uint64_t retire(uint64_t upToFrame);

        // Hands back every held and waiting payload once, then empties the allocator and ends any reservation, so fresh
        // slots start from zero again. `onNative` runs under the allocator's lock and must not call the allocator.
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

        // A slot never handed out is Free.
        SlotState state(uint32_t index);

        size_t assignedCount();

        // A reserved block alone leaves it empty.
        bool empty();

      private:
        struct Slot
        {
            SlotState state = SlotState::Free;
            uint64_t owner = 0;
            uint32_t rangeCount = 0; // Nonzero only on the first slot of a range.
            bool inRange = false;    // Every slot of a range, its first included.
            void* native = nullptr;
        };

        enum class PendingKind : uint8_t
        {
            Single,
            Range,
            Payload, // A write replaced it; its slot stays assigned.
        };

        struct Pending
        {
            uint64_t frame;
            uint32_t index;
            uint32_t count; // Zero for a replaced payload.
            PendingKind kind;
            void* native;               // Single or Payload; may be null.
            std::vector<void*> natives; // The non-null payloads of a Range.
        };

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
        std::vector<Slot> m_slots;
        std::vector<uint32_t> m_free;   // Reused before a fresh slot.
        std::vector<Pending> m_pending; // In release order.
        uint32_t m_reserved = 0;
    };
} // namespace rojoRHI::core
