//----------------------------------------------------------------------------------------------------------------------
/// @file HandlePool.h
/// @brief Declares the generation-checked pool that issues and validates one handle type.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Handles.h>

#include "Base/Assert.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
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

    /// One pool of slots for one handle type. A handle packs a 32-bit slot index, a 24-bit generation and an 8-bit type
    /// tag; slot zero and tag zero are never issued, so the null handle is never valid. Releasing a slot advances its
    /// generation at once, so the old handle is rejected immediately, and the slot is reused only after the frame it
    /// was released in has retired. A slot whose generation reaches the limit is never reused. All operations take one
    /// mutex; the record pointers a lookup returns are stable until the slot is released.
    template<typename Handle, typename Record> class HandlePool
    {
      public:
        /// The largest generation a slot can hold; a slot released at this generation is retired for good.
        static constexpr uint32_t kMaxGeneration = (1u << 24) - 1;

        /// Creates an empty pool. `maxGeneration` exists so a test can reach the limit without 2^24 reuses.
        explicit HandlePool(uint32_t maxGeneration = kMaxGeneration) : maxGeneration_(maxGeneration)
        {
            ROJORHI_INTERNAL_ASSERT(maxGeneration <= kMaxGeneration, "a generation wider than 24 bits would corrupt the type tag");
            slots_.emplace_back(); // Slot zero is never issued.
        }

        /// Stores `record` in a free slot, or a new one, and returns the handle naming it.
        Handle allocate(Record record)
        {
            std::lock_guard lock(mutex_);
            uint32_t index = 0;
            if (!free_.empty())
            {
                index = free_.back();
                free_.pop_back();
            }
            else
            {
                index = static_cast<uint32_t>(slots_.size());
                slots_.emplace_back();
            }
            Slot& slot = slots_[index];
            slot.record = std::move(record);
            slot.live = true;
            return Handle{ pack(index, slot.generation) };
        }

        /// Destroys the slot `handle` names: its generation advances now, and the slot becomes reusable once
        /// `retire` is called with a frame at or past `frame`. Returns the lookup status; anything but Ok released nothing.
        LookupStatus release(Handle handle, uint64_t frame)
        {
            std::lock_guard lock(mutex_);
            return releaseLocked(handle, frame);
        }

        /// Releases the slot `handle` names as `release` does, taking the frame from `frameOf()`, which runs under the pool's
        /// lock so a `retire` cannot slip between reading the frame and recording the release. `frameOf` must not call the
        /// pool. Issues VU-rojo-core-0001, 0002 or 0003 when the handle breaks a rule; the null handle is never accepted.
        template<typename FrameSupplier> void checkedRelease(Handle handle, FrameSupplier&& frameOf)
        {
            [[maybe_unused]] LookupStatus status = LookupStatus::Ok;
            {
                std::lock_guard lock(mutex_);
                status = releaseLocked(handle, frameOf());
            }
            ROJORHI_VU(status != LookupStatus::Null, "VU-rojo-core-0001", "a null handle was passed where a live one is required");
            ROJORHI_VU(status != LookupStatus::StaleGeneration, "VU-rojo-core-0002", "handle {:#x} names a destroyed resource", handle.bits);
            ROJORHI_VU(status != LookupStatus::WrongType, "VU-rojo-core-0003", "handle {:#x} has the type tag of another resource type", handle.bits);
        }

        /// Retires every slot released at a frame up to and including `upToFrame`: `onRetire` receives its record, then the
        /// record is cleared and the slot becomes available for reuse unless its generation reached the limit. `onRetire`
        /// runs under the pool's lock and must not call the pool.
        template<typename OnRetire> void retire(uint64_t upToFrame, OnRetire&& onRetire)
        {
            std::lock_guard lock(mutex_);
            std::erase_if(pending_, [&](const Pending& entry) {
                if (entry.frame > upToFrame)
                {
                    return false;
                }
                Slot& slot = slots_[entry.index];
                onRetire(slot.record);
                slot.record = Record{};
                if (entry.reusable)
                {
                    free_.push_back(entry.index);
                }
                return true;
            });
        }

        /// Retires as the two-argument overload does, with nothing to do for each record.
        void retire(uint64_t upToFrame)
        {
            retire(upToFrame, [](Record&) {});
        }

        /// Finds the record `handle` names, or reports which rule it breaks.
        LookupResult<Record> lookup(Handle handle)
        {
            std::lock_guard lock(mutex_);
            return find(handle);
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

        /// Calls `visit` with the record of every live slot while holding the pool's lock; `visit` must not call the pool.
        template<typename Visitor> void forEachLive(Visitor&& visit)
        {
            std::lock_guard lock(mutex_);
            for (Slot& slot : slots_)
            {
                if (slot.live)
                {
                    visit(slot.record);
                }
            }
        }

        /// Returns the number of live slots.
        size_t liveCount()
        {
            std::lock_guard lock(mutex_);
            size_t count = 0;
            for (const Slot& slot : slots_)
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
            Slot& slot = slots_[index];
            slot.live = false;
            // The record stays until retire: a backend releases the native object it describes only then.
            if (slot.generation >= maxGeneration_)
            {
                pending_.push_back(Pending{ frame, index, false }); // The generation cannot advance, so the slot is never reused.
                return status;
            }
            ++slot.generation;
            pending_.push_back(Pending{ frame, index, true });
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
            if (index == 0 || index >= slots_.size() || !slots_[index].live || slots_[index].generation != generation)
            {
                return { LookupStatus::StaleGeneration, nullptr };
            }
            return { LookupStatus::Ok, &slots_[index].record };
        }

        std::mutex mutex_;
        uint32_t maxGeneration_;
        std::deque<Slot> slots_;
        std::vector<uint32_t> free_;
        std::vector<Pending> pending_;
    };
} // namespace rojoRHI::core
