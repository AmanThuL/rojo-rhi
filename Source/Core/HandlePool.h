//----------------------------------------------------------------------------------------------------------------------
/// @file HandlePool.h
/// @brief Declares the generation-checked pool that issues and validates one handle type.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/NativeTable.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI::core {
    // Zero is never a tag.
    template<typename Handle> constexpr uint8_t handleTypeTag();

    template<> constexpr uint8_t handleTypeTag<Buffer>()
    {
        return 1;
    }
    template<> constexpr uint8_t handleTypeTag<Texture>()
    {
        return 2;
    }
    template<> constexpr uint8_t handleTypeTag<Pipeline>()
    {
        return 3;
    }
    template<> constexpr uint8_t handleTypeTag<ShaderLibrary>()
    {
        return 4;
    }
    template<> constexpr uint8_t handleTypeTag<Heap>()
    {
        return 5;
    }
    template<> constexpr uint8_t handleTypeTag<Fence>()
    {
        return 6;
    }
    template<> constexpr uint8_t handleTypeTag<Upscaler>()
    {
        return 7;
    }

    enum class LookupStatus : uint8_t
    {
        Ok,
        Null,            // VU-rojo-core-0001
        StaleGeneration, // Destroyed or reused since the handle was issued (VU-rojo-core-0002).
        WrongType        // VU-rojo-core-0003
    };

    template<typename Record> struct LookupResult
    {
        LookupStatus status = LookupStatus::Null;
        Record* record = nullptr;
    };

    template<typename Handle, typename Record> struct LiveSlot
    {
        Handle handle;
        Record* record;
    };

    inline Error tableFull(std::string_view type, uint32_t capacity)
    {
        return Error{ ErrorCategory::OutOfMemory, std::format("the {} table of {} objects is full", type, capacity - 1) };
    }

    // Slots, slot zero included and at most `NativeTable::kCapacity`; one argument, so a set of pools is built from one
    // value.
    struct PoolCapacity
    {
        uint32_t slots = NativeTable::kCapacity;
    };

    // A handle packs a 32-bit slot index, a 24-bit generation and an 8-bit type tag; slot zero and tag zero are never
    // issued, so the null handle is never valid. Releasing a slot advances its generation at once, so the old handle is
    // rejected immediately, and the slot is reused only after the frame it was released in has retired; a slot whose
    // generation reaches the limit is never reused. One mutex guards everything except `native`, which reads the
    // `NativeTable` lock-free. A lookup's record pointer is stable until the slot is released. An allocation past the
    // table's capacity issues the null handle.
    template<typename Handle, typename Record> class HandlePool
    {
      public:
        using HandleType = Handle;

        // A slot released at this generation is retired for good.
        static constexpr uint32_t kMaxGeneration = (1u << 24) - 1;

        // `maxGeneration` and `capacity` exist so a test can reach either limit without 2^24 reuses or 2^22 objects.
        explicit HandlePool(uint32_t maxGeneration = kMaxGeneration, uint32_t capacity = NativeTable::kCapacity)
            : m_maxGeneration(maxGeneration), m_table(capacity)
        {
            ROJORHI_INTERNAL_ASSERT(maxGeneration <= kMaxGeneration, "a generation wider than 24 bits would corrupt the type tag");
            m_slots.emplace_back(); // Slot zero is never issued.
        }

        explicit HandlePool(PoolCapacity capacity) : HandlePool(kMaxGeneration, capacity.slots)
        {
        }

        // Slot zero included.
        uint32_t capacity() const
        {
            return m_table.capacity();
        }

        // Checks nothing; a backend may encode the index in its own values.
        static uint32_t slotIndex(Handle handle)
        {
            return indexOf(handle.bits);
        }

        // `counted` false is for a record that stands for no native creation. A full pool returns the null handle,
        // counts nothing and leaves `record` unmoved, so the caller still owns its payload.
        Handle allocate(Record&& record, bool counted = true)
        {
            std::lock_guard lock(m_mutex);
            uint32_t index = 0;
            if (!m_free.empty())
            {
                index = m_free.back();
                m_free.pop_back();
            }
            else if (m_slots.size() >= m_table.capacity())
            {
                return Handle{};
            }
            else
            {
                index = static_cast<uint32_t>(m_slots.size());
                m_slots.emplace_back();
            }
            Slot& slot = m_slots[index];
            slot.record = std::move(record);
            slot.live = true;
            for (std::atomic<uint64_t>* counter : m_counted)
            {
                if (counted && counter != nullptr)
                {
                    counter->fetch_add(1, std::memory_order_relaxed);
                }
            }
            m_table.publish(index, slot.record.native);
            return Handle{ pack(index, slot.generation) };
        }

        // Relaxed adds; a null entry is skipped. The counters must outlive the pool, and are wired once before it is
        // used.
        void countInto(std::atomic<uint64_t>* first, std::atomic<uint64_t>* second = nullptr)
        {
            std::lock_guard lock(m_mutex);
            m_counted = { first, second };
        }

        // The generation advances now; the slot is reusable once `retire` reaches `frame`. Anything but Ok released
        // nothing.
        LookupStatus release(Handle handle, uint64_t frame)
        {
            std::lock_guard lock(m_mutex);
            return releaseLocked(handle, frame);
        }

        // `frameOf` runs under the pool's lock, so a `retire` cannot slip between reading the frame and recording the
        // release; it must not call the pool. The null handle is never accepted.
        template<typename FrameSupplier> void checkedRelease(Handle handle, FrameSupplier&& frameOf)
        {
            [[maybe_unused]] LookupStatus status = LookupStatus::Ok;
            {
                std::lock_guard lock(m_mutex);
                status = releaseLocked(handle, frameOf());
            }
            ROJORHI_VU(status != LookupStatus::Null, "VU-rojo-core-0001", "a null handle was passed where a live one is required");
            ROJORHI_VU(status != LookupStatus::StaleGeneration, "VU-rojo-core-0002", "handle {:#x} names a destroyed resource", handle.bits);
            ROJORHI_VU(status != LookupStatus::WrongType, "VU-rojo-core-0003", "handle {:#x} has the type tag of another resource type", handle.bits);
        }

        // Per slot: the native payload reads null, then `onRetire` gets the record, then the slot is freed unless its
        // generation reached the limit. `onRetire` runs under the pool's lock and must not call the pool.
        template<typename OnRetire> void retire(uint64_t upToFrame, OnRetire&& onRetire)
        {
            std::lock_guard lock(m_mutex);
            std::erase_if(m_pending, [&](const Pending& entry) {
                if (entry.frame > upToFrame)
                {
                    return false;
                }
                Slot& slot = m_slots[entry.index];
                clearNative(entry.index, slot);
                onRetire(slot.record);
                slot.record = Record{};
                if (entry.reusable)
                {
                    m_free.push_back(entry.index);
                }
                return true;
            });
        }

        // Hands over every live and every pending record exactly once, each after its native payload reads null;
        // afterwards no issued handle is valid and no slot is reused. `onRecord` runs under the pool's lock and must
        // not call the pool.
        template<typename OnRecord> void drain(OnRecord&& onRecord)
        {
            std::lock_guard lock(m_mutex);
            for (const Pending& entry : m_pending)
            {
                Slot& slot = m_slots[entry.index];
                clearNative(entry.index, slot);
                onRecord(slot.record);
                slot.record = Record{};
            }
            m_pending.clear();
            m_free.clear();
            for (uint32_t index = 1; index < m_slots.size(); ++index)
            {
                Slot& slot = m_slots[index];
                if (slot.live)
                {
                    clearNative(index, slot);
                    onRecord(slot.record);
                    slot.record = Record{};
                    slot.live = false;
                }
            }
        }

        void retire(uint64_t upToFrame)
        {
            retire(upToFrame, [](Record&) {});
        }

        // One acquire load, no lock, no check: valid for a live handle a Debug build has looked up, or one valid usage
        // keeps alive until its frame retires. A record's `native` does not change between `allocate` and retirement;
        // the null handle reads slot zero, which is always null.
        void* native(Handle handle) const
        {
            return m_table.load(slotIndex(handle));
        }

        LookupResult<Record> lookup(Handle handle)
        {
            std::lock_guard lock(m_mutex);
            return find(handle);
        }

        // Whatever generation the slot holds, for a backend whose values carry an index and no generation.
        std::optional<LiveSlot<Handle, Record>> liveAt(uint32_t index)
        {
            std::lock_guard lock(m_mutex);
            if (index == 0 || index >= m_slots.size() || !m_slots[index].live)
            {
                return std::nullopt;
            }
            return LiveSlot<Handle, Record>{ Handle{ pack(index, m_slots[index].generation) }, &m_slots[index].record };
        }

        // A null handle passes, as a null record, when `allowNull` is set.
        Record* checked(Handle handle, [[maybe_unused]] bool allowNull)
        {
            const LookupResult<Record> result = lookup(handle);
            ROJORHI_VU(result.status != LookupStatus::Null || allowNull, "VU-rojo-core-0001", "a null handle was passed where a live one is required");
            ROJORHI_VU(result.status != LookupStatus::StaleGeneration, "VU-rojo-core-0002", "handle {:#x} names a destroyed resource", handle.bits);
            ROJORHI_VU(result.status != LookupStatus::WrongType, "VU-rojo-core-0003", "handle {:#x} has the type tag of another resource type", handle.bits);
            return result.record;
        }

        // `change` runs under the pool's lock, only for Ok, and must not call the pool: a record's counters change
        // without a race.
        template<typename Change> LookupStatus update(Handle handle, Change&& change)
        {
            std::lock_guard lock(m_mutex);
            const LookupResult<Record> result = find(handle);
            if (result.status == LookupStatus::Ok)
            {
                change(*result.record);
            }
            return result.status;
        }

        // `visit` runs under the pool's lock and must not call the pool.
        template<typename Visitor> void forEachLive(Visitor&& visit)
        {
            std::lock_guard lock(m_mutex);
            for (Slot& slot : m_slots)
            {
                if (slot.live)
                {
                    visit(slot.record);
                }
            }
        }

        bool empty()
        {
            std::lock_guard lock(m_mutex);
            return m_pending.empty() && std::none_of(m_slots.begin(), m_slots.end(), [](const Slot& slot) { return slot.live; });
        }

        size_t liveCount()
        {
            std::lock_guard lock(m_mutex);
            size_t count = 0;
            for (const Slot& slot : m_slots)
            {
                count += slot.live ? 1 : 0;
            }
            return count;
        }

      private:
        LookupStatus releaseLocked(Handle handle, uint64_t frame)
        {
            const LookupStatus status = find(handle).status;
            if (status != LookupStatus::Ok)
            {
                return status;
            }
            const uint32_t index = indexOf(handle.bits);
            Slot& slot = m_slots[index];
            slot.live = false;
            // The record stays until retire: a backend releases the native object it describes only then.
            if (slot.generation >= m_maxGeneration)
            {
                m_pending.push_back(Pending{ frame, index, false }); // The generation cannot advance, so the slot is never reused.
                return status;
            }
            ++slot.generation;
            m_pending.push_back(Pending{ frame, index, true });
            return status;
        }

        struct Slot
        {
            Record record{};
            uint32_t generation = 0;
            bool live = false;
        };

        struct Pending
        {
            uint64_t frame;
            uint32_t index;
            bool reusable;
        };

        // Clears the slot's native payload before its record leaves the pool, so no reader is handed a payload being
        // released.
        void clearNative(uint32_t index, [[maybe_unused]] const Slot& slot)
        {
            ROJORHI_INTERNAL_ASSERT(slot.record.native == m_table.load(index), "a record's native payload changed after it was published");
            m_table.publish(index, nullptr);
        }

        static uint64_t pack(uint32_t index, uint32_t generation)
        {
            return static_cast<uint64_t>(index) | (static_cast<uint64_t>(generation) << 32) | (static_cast<uint64_t>(handleTypeTag<Handle>()) << 56);
        }

        static uint32_t indexOf(uint64_t bits)
        {
            return static_cast<uint32_t>(bits);
        }

        LookupResult<Record> find(Handle handle)
        {
            if (handle.bits == 0)
            {
                return { LookupStatus::Null, nullptr };
            }
            if (static_cast<uint8_t>(handle.bits >> 56) != handleTypeTag<Handle>())
            {
                return { LookupStatus::WrongType, nullptr };
            }
            const uint32_t index = indexOf(handle.bits);
            const uint32_t generation = static_cast<uint32_t>(handle.bits >> 32) & kMaxGeneration;
            if (index == 0 || index >= m_slots.size() || !m_slots[index].live || m_slots[index].generation != generation)
            {
                return { LookupStatus::StaleGeneration, nullptr };
            }
            return { LookupStatus::Ok, &m_slots[index].record };
        }

        std::mutex m_mutex;
        std::array<std::atomic<uint64_t>*, 2> m_counted = {};
        uint32_t m_maxGeneration;
        std::deque<Slot> m_slots;
        std::vector<uint32_t> m_free;
        std::vector<Pending> m_pending;
        NativeTable m_table;
    };
} // namespace rojoRHI::core
