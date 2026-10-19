//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Timestamps.h
/// @brief Declares the counter heaps a Metal 4 device times scopes with: a pair of timestamps per scope, per frame slot.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Counters.h"
#include "Core/ScopeRecord.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace rojoRHI {
    struct CommandListImpl;
} // namespace rojoRHI

namespace rojoRHI::metal4 {
    /// The timestamp storage of a device: per frame slot, up to `kSlotHeaps` counter heaps of `kHeapEntries` timestamps and a
    /// cursor. A scope claims a pair of entries with one atomic add on its slot's cursor; a pair never straddles two heaps. A
    /// heap is created on the first claim that needs it and published with a compare-exchange, so recording takes no lock and
    /// a device that records no scope holds no heap. Heaps live until `release`. At the frame wait the scope-time hook
    /// resolves each used heap of the reused slot once, and the slot's reuse invalidates the used ranges and resets the cursor.
    class Timestamps
    {
      public:
        static constexpr uint32_t kHeapEntries = 4096; ///< The timestamps of one heap, the most Metal allows in one.
        static constexpr uint32_t kSlotHeaps = 8;      ///< The heaps of one frame slot, of the 32 Metal allows per process or on the system.
        static constexpr uint32_t kSlots = 3;          ///< The frame slots, as many as a device may have.

        /// Keeps `device`, which the caller owns, for heap creation and reads its timestamp frequency.
        void create(MTL::Device* device);

        /// Claims the next pair of entries of frame slot `slot` and returns the index of its first, or `core::kNoTimestamp`
        /// when the slot's heaps are full or Metal refused the heap the pair needs. Creates and publishes that heap when it
        /// does not exist yet, counting one `nativeCreateCalls` in `counts` for a heap it keeps. A heap Metal refused is not
        /// asked for again until the slot's reuse, so a refusal costs one failed request per heap and frame (more only
        /// when threads race to create it).
        /// Free-threaded, lock-free.
        uint32_t claim(uint32_t slot, core::DeviceCounts& counts);

        /// Returns the heap holding entry `index` of frame slot `slot`, which a claim returned.
        MTL4::CounterHeap* heapFor(uint32_t slot, uint32_t index) const
        {
            return heap(slot, index / kHeapEntries);
        }

        /// Returns heap `number` of frame slot `slot`, or null when none was created.
        MTL4::CounterHeap* heap(uint32_t slot, uint32_t number) const
        {
            return m_slots[slot].heaps[number].load(std::memory_order_acquire);
        }

        /// Fills `report[i]` with the GPU duration of `records[i]`, a scope of the frame that retired from slot `slot`. The
        /// first call after the slot's reuse resolves each heap the slot used, once, counting one `nativeHostCalls` in
        /// `counts` per heap; later calls for other queues read what it resolved. A record without a timestamp, an entry Metal
        /// reports as unwritten and an end before its begin give zero. Opens an autorelease pool. The scope-time hook calls it.
        void resolve(uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report, core::DeviceCounts& counts);

        /// Readies frame slot `slot` for its next frame, after its frame wait and its resolve: invalidates each used range,
        /// counting one `nativeHostCalls` in `counts` per heap, lets go of what `resolve` read, resets the cursor, and logs one
        /// warning naming the retired frame when any of its reported scopes went untimed, with the capacity or a refused heap
        /// as the reason when one applies. It clears the slot's refusals, since Metal's limit holds per process or on the
        /// system and may have room by the next frame. Opens an autorelease pool.
        void reuse(uint32_t slot, core::DeviceCounts& counts);

        /// Releases every heap and what `resolve` kept, at device destruction; the caller holds an autorelease pool and no queue
        /// holds work that writes the heaps.
        void release();

      private:
        /// The timestamp storage of one frame slot.
        struct Slot
        {
            std::array<std::atomic<MTL4::CounterHeap*>, kSlotHeaps> heaps = {}; ///< The heaps, created on first need.
            std::atomic<uint32_t> cursor = 0;                                   ///< The next unclaimed entry.
            std::array<std::atomic<bool>, kSlotHeaps> refused = {};             ///< Each heap Metal refused this frame; asked again after reuse.
            std::array<NS::Data*, kSlotHeaps> resolved = {};                    ///< Each used heap's entries, retained, from the first resolve.
            bool resolvedFrame = false;                                         ///< True once the frame retiring from this slot was resolved.
            uint64_t frame = 0;                                                 ///< The frame the reported scopes belong to.
            uint64_t untimed = 0;                                               ///< Reported scopes of that frame with no timestamp.
        };

        /// Creates heap `number` of frame slot `slot` and publishes it, unless another claim did first; returns false when
        /// Metal refused it.
        bool createHeap(uint32_t slot, uint32_t number, core::DeviceCounts& counts);

        /// Returns the entries of frame slot `slot` claims have used: the cursor, capped at the slot's capacity.
        uint32_t used(const Slot& slot) const;

        /// Returns the duration in milliseconds between the timestamps of entries `index` and `index + 1`, or zero.
        double milliseconds(const Slot& slot, uint32_t index) const;

        MTL::Device* m_device = nullptr;           ///< The device heaps are created on; the device owns it.
        uint64_t m_frequency = 0;                  ///< GPU timestamp ticks per second.
        std::array<Slot, kSlots> m_slots;          ///< The storage of each frame slot.
        std::atomic<bool> m_refusalLogged = false; ///< True once a refused heap was logged; each device logs one.
    };

    /// The granularity of a timestamp a render encoder writes for a scope inside a pass.
    constexpr MTL4::TimestampGranularity kInPassGranularity = MTL4::TimestampGranularityPrecise;

    /// Claims a pair of entries of `list`'s frame slot for the scope the core recorded last on `list`, stores it on that
    /// record for the resolve, pushes it on the list's pair stack and returns its first index, or `core::kNoTimestamp` when
    /// the slot gives none. One atomic add; the heap is created on its first claim. Free-threaded across lists, lock-free.
    uint32_t claimPair(CommandListImpl& list);

    /// Pops the innermost pair of `list`'s pair stack and returns the index of its end entry, or `core::kNoTimestamp` when the
    /// scope claimed none.
    uint32_t popPairEnd(CommandListImpl& list);

    /// Writes entry `index` of `list`'s frame slot on its command buffer, which takes it only between encoders; writes nothing
    /// for `core::kNoTimestamp`. Counts one native call and opens an autorelease pool, because Metal autoreleases inside the
    /// write.
    void writeBetweenEncoders(CommandListImpl& list, uint32_t index);

    /// Writes entry `index` of `list`'s frame slot on `encoder`, the open render encoder of its pass, after the fragment stage
    /// of the work recorded before it, at `kInPassGranularity`; writes nothing for `core::kNoTimestamp`. Counts one native
    /// call.
    void writeInPass(CommandListImpl& list, MTL4::RenderCommandEncoder* encoder, uint32_t index);

    /// The scope-time hook of a Metal 4 device: `context` is its `DeviceImpl`, whose timestamps resolve the durations.
    void scopeTimes(void* context, uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report);
} // namespace rojoRHI::metal4
