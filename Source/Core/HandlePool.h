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
    /// Returns the type tag stored in the top bits of a handle of type `Handle`; zero is never a tag.
    template<typename Handle> constexpr uint8_t handleTypeTag();

    /// Tag of Buffer handles.
    template<> constexpr uint8_t handleTypeTag<Buffer>()
    {
        return 1;
    }
    /// Tag of Texture handles.
    template<> constexpr uint8_t handleTypeTag<Texture>()
    {
        return 2;
    }
    /// Tag of Pipeline handles.
    template<> constexpr uint8_t handleTypeTag<Pipeline>()
    {
        return 3;
    }
    /// Tag of ShaderLibrary handles.
    template<> constexpr uint8_t handleTypeTag<ShaderLibrary>()
    {
        return 4;
    }
    /// Tag of Heap handles.
    template<> constexpr uint8_t handleTypeTag<Heap>()
    {
        return 5;
    }
    /// Tag of Fence handles.
    template<> constexpr uint8_t handleTypeTag<Fence>()
    {
        return 6;
    }
    /// Tag of Upscaler handles.
    template<> constexpr uint8_t handleTypeTag<Upscaler>()
    {
        return 7;
    }

    /// Why a lookup succeeded or which valid-usage rule it failed.
    enum class LookupStatus : uint8_t
    {
        Ok,              ///< The handle names a live slot.
        Null,            ///< The handle is the null handle (VU-rojo-core-0001).
        StaleGeneration, ///< The slot was destroyed or reused since the handle was issued (VU-rojo-core-0002).
        WrongType        ///< The handle's type tag is not this pool's type (VU-rojo-core-0003).
    };

    /// The outcome of a lookup: the status and, for Ok, the slot's record.
    template<typename Record> struct LookupResult
    {
        LookupStatus status = LookupStatus::Null; ///< Which rule failed, or Ok.
        Record* record = nullptr;                 ///< The slot's record when the status is Ok, otherwise null.
    };

    /// The live slot a slot index names: the handle that names it now and its record.
    template<typename Handle, typename Record> struct LiveSlot
    {
        Handle handle;  ///< The handle that names the slot at its current generation.
        Record* record; ///< The slot's record.
    };

    /// Returns the `OutOfMemory` error of a creation that found the native table of `type` objects full at `capacity` slots,
    /// slot zero included.
    inline Error tableFull(std::string_view type, uint32_t capacity)
    {
        return Error{ ErrorCategory::OutOfMemory, std::format("the {} table of {} objects is full", type, capacity - 1) };
    }

    /// The number of slots a pool is created with, slot zero included, as one argument so a set of pools is built from one
    /// value.
    struct PoolCapacity
    {
        uint32_t slots = NativeTable::kCapacity; ///< Slots, at most `NativeTable::kCapacity`.
    };

    /// One pool of slots for one handle type. A handle packs a 32-bit slot index, a 24-bit generation and an 8-bit type
    /// tag; slot zero and tag zero are never issued, so the null handle is never valid. Releasing a slot advances its
    /// generation at once, so the old handle is rejected immediately, and the slot is reused only after the frame it
    /// was released in has retired. A slot whose generation reaches the limit is never reused. All operations take one
    /// mutex; the record pointers a lookup returns are stable until the slot is released. Beside the records the pool keeps a
    /// `NativeTable` of each slot's `native` payload, which `native` reads without the mutex; its capacity, 2^22 slots unless a
    /// test passes less, bounds the slot indices, and an allocation past it issues the null handle.
    template<typename Handle, typename Record> class HandlePool
    {
      public:
        using HandleType = Handle; ///< The handle type this pool issues.

        /// The largest generation a slot can hold; a slot released at this generation is retired for good.
        static constexpr uint32_t kMaxGeneration = (1u << 24) - 1;

        /// Creates an empty pool of at most `capacity` slots, slot zero included. `maxGeneration` and `capacity` exist so a test
        /// can reach either limit without 2^24 reuses or 2^22 objects.
        explicit HandlePool(uint32_t maxGeneration = kMaxGeneration, uint32_t capacity = NativeTable::kCapacity)
            : m_maxGeneration(maxGeneration), m_table(capacity)
        {
            ROJORHI_INTERNAL_ASSERT(maxGeneration <= kMaxGeneration, "a generation wider than 24 bits would corrupt the type tag");
            m_slots.emplace_back(); // Slot zero is never issued.
        }

        /// Creates an empty pool of `capacity` slots with the default generation limit.
        explicit HandlePool(PoolCapacity capacity) : HandlePool(kMaxGeneration, capacity.slots)
        {
        }

        /// Returns the number of slots the pool can issue, slot zero included.
        uint32_t capacity() const
        {
            return m_table.capacity();
        }

        /// Returns the slot index of `handle`, which a backend may encode in its own values; it does not check the handle.
        static uint32_t slotIndex(Handle handle)
        {
            return indexOf(handle.bits);
        }

        /// Stores `record` in a free slot, or a new one, publishes its `native` payload and returns the handle naming it.
        /// `counted` false leaves the counters of `countInto` alone, for a record that stands for no native creation. When no
        /// slot is free and a new one would reach the capacity, it returns the null handle, counts nothing and leaves `record`
        /// unmoved, so the caller still owns its payload.
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

        /// Makes every later `allocate` that counts add one to each of `counters`, relaxed; a null entry is skipped. The counters
        /// must outlive the pool. The device core wires them once, before the pool is used.
        void countInto(std::atomic<uint64_t>* first, std::atomic<uint64_t>* second = nullptr)
        {
            std::lock_guard lock(m_mutex);
            m_counted = { first, second };
        }

        /// Destroys the slot `handle` names: its generation advances now, and the slot becomes reusable once
        /// `retire` is called with a frame at or past `frame`. Returns the lookup status; anything but Ok released nothing.
        LookupStatus release(Handle handle, uint64_t frame)
        {
            std::lock_guard lock(m_mutex);
            return releaseLocked(handle, frame);
        }

        /// Releases the slot `handle` names as `release` does, taking the frame from `frameOf()`, which runs under the pool's
        /// lock so a `retire` cannot slip between reading the frame and recording the release. `frameOf` must not call the
        /// pool. Issues VU-rojo-core-0001, 0002 or 0003 when the handle breaks a rule; the null handle is never accepted.
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

        /// Retires every slot released at a frame up to and including `upToFrame`: the slot's native payload reads null, then
        /// `onRetire` receives its record, then the record is cleared and the slot becomes available for reuse unless its
        /// generation reached the limit. `onRetire` runs under the pool's lock and must not call the pool.
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

        /// Hands `onRecord` the record of every live slot and of every slot released but not yet retired, each exactly once, and
        /// empties the pool: every native payload reads null before its record is handed over, no handle it issued is valid
        /// afterwards and no slot is reused.
        /// `onRecord` runs under the pool's lock and must not call the pool.
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

        /// Retires as the two-argument overload does, with nothing to do for each record.
        void retire(uint64_t upToFrame)
        {
            retire(upToFrame, [](Record&) {});
        }

        /// Returns the native payload of the slot `handle` names with one acquire load, taking no lock and checking nothing. It
        /// is valid for a live handle that a Debug build has already looked up, or one that valid usage keeps alive until its
        /// frame retires; a record's `native` does not change between `allocate` and retirement. The null handle reads slot
        /// zero, which is always null.
        void* native(Handle handle) const
        {
            return m_table.load(slotIndex(handle));
        }

        /// Finds the record `handle` names, or reports which rule it breaks.
        LookupResult<Record> lookup(Handle handle)
        {
            std::lock_guard lock(m_mutex);
            return find(handle);
        }

        /// Finds the live slot at `index`, whatever generation it holds, for a backend whose values carry a slot index and no
        /// generation; empty for slot zero, an index past the pool and a slot that is not live.
        std::optional<LiveSlot<Handle, Record>> liveAt(uint32_t index)
        {
            std::lock_guard lock(m_mutex);
            if (index == 0 || index >= m_slots.size() || !m_slots[index].live)
            {
                return std::nullopt;
            }
            return LiveSlot<Handle, Record>{ Handle{ pack(index, m_slots[index].generation) }, &m_slots[index].record };
        }

        /// Looks `handle` up and issues VU-rojo-core-0001, 0002 or 0003 when it breaks a rule; a null handle passes when
        /// `allowNull` is set. Returns the record, or null for an allowed null handle.
        Record* checked(Handle handle, [[maybe_unused]] bool allowNull)
        {
            const LookupResult<Record> result = lookup(handle);
            ROJORHI_VU(result.status != LookupStatus::Null || allowNull, "VU-rojo-core-0001", "a null handle was passed where a live one is required");
            ROJORHI_VU(result.status != LookupStatus::StaleGeneration, "VU-rojo-core-0002", "handle {:#x} names a destroyed resource", handle.bits);
            ROJORHI_VU(result.status != LookupStatus::WrongType, "VU-rojo-core-0003", "handle {:#x} has the type tag of another resource type", handle.bits);
            return result.record;
        }

        /// Calls `change` with the record `handle` names while holding the pool's lock, and returns the lookup status; `change`
        /// runs only for Ok and must not call the pool. It is how a record's counters change without a race.
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

        /// Calls `visit` with the record of every live slot while holding the pool's lock; `visit` must not call the pool.
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

        /// Returns true when no slot is live and none is released but not yet retired.
        bool empty()
        {
            std::lock_guard lock(m_mutex);
            return m_pending.empty() && std::none_of(m_slots.begin(), m_slots.end(), [](const Slot& slot) { return slot.live; });
        }

        /// Returns the number of live slots.
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

        // Clears the slot's native payload before its record leaves the pool, so no reader is handed a payload being released.
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
