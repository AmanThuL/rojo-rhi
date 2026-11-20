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
#include <optional>
#include <span>
#include <string>

namespace rojoRHI {
    struct CommandListImpl;
} // namespace rojoRHI

namespace rojoRHI::metal4 {
    // A scope claims a pair of entries with one atomic add on its slot's cursor; a pair never straddles two heaps. A
    // heap is created on the first claim that needs it and published with a compare-exchange, so recording takes no
    // lock. At the frame wait the scope-time hook resolves each used heap of the reused slot once; the slot's reuse
    // invalidates the used ranges.
    class Timestamps
    {
      public:
        static constexpr uint32_t kHeapEntries = 4096; // The most Metal allows in one heap.
        static constexpr uint32_t kSlotHeaps = 8;      // Of the 32 Metal allows per process or on the system.
        static constexpr uint32_t kSlots = 3;          // As many as a device may have.

        // The caller owns `device`.
        void create(MTL::Device* device);

        // Returns `core::kNoTimestamp` when the slot's heaps are full or Metal refused the heap the pair needs. A
        // refused heap is not asked for again until the slot's reuse, so a refusal costs one failed request per heap
        // and frame. Free-threaded, lock-free.
        uint32_t claim(uint32_t slot, core::DeviceCounts& counts);

        MTL4::CounterHeap* heapFor(uint32_t slot, uint32_t index) const
        {
            return heap(slot, index / kHeapEntries);
        }

        MTL4::CounterHeap* heap(uint32_t slot, uint32_t number) const
        {
            return m_slots[slot].heaps[number].load(std::memory_order_acquire);
        }

        // Warns once per device when Metal refused a heap since the device was created, and does nothing otherwise or on a
        // later call. A claim only records the refusal, because the sink takes a lock and recording takes none (ADR 0041);
        // `Device::beginFrame` calls this. Free-threaded; one acquire load when nothing was refused.
        void reportRefusal();

        // The first call after the slot's reuse resolves each used heap once; later calls for other queues read what it
        // resolved. Opens an autorelease pool.
        void resolve(uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report, core::DeviceCounts& counts);

        // After the slot's frame wait and resolve. It clears the slot's refusals, since Metal's limit holds per process
        // or on the system and may have room by the next frame. Opens an autorelease pool.
        void reuse(uint32_t slot, core::DeviceCounts& counts);

        // At device destruction; the caller holds an autorelease pool and no queue holds work that writes the heaps.
        void release();

      private:
        struct Slot
        {
            std::array<std::atomic<MTL4::CounterHeap*>, kSlotHeaps> heaps = {}; // Created on first need.
            std::atomic<uint32_t> cursor = 0;
            std::array<std::atomic<bool>, kSlotHeaps> refused = {}; // Asked for again after reuse.
            std::array<NS::Data*, kSlotHeaps> resolved = {};        // Retained, from the first resolve.
            bool resolvedFrame = false;
            uint64_t frame = 0;
            uint64_t untimed = 0;
            uint64_t unwritten = 0;
        };

        // Unless another claim published one first; false when Metal refused it.
        bool createHeap(uint32_t slot, uint32_t number, core::DeviceCounts& counts);

        // The cursor, capped at the slot's capacity.
        uint32_t used(const Slot& slot) const;

        // `std::nullopt` when an entry is zero or `MTL::CounterErrorValue` or the end precedes the begin. The caller
        // handles a zero frequency.
        std::optional<double> milliseconds(const Slot& slot, uint32_t index) const;

        MTL::Device* m_device = nullptr; // The device owns it.
        uint64_t m_frequency = 0;        // Ticks per second.
        std::array<Slot, kSlots> m_slots;
        // The first refusal of the device, recorded by the claim that met it and reported by `reportRefusal`. Free of locks:
        // the claim that moves `m_refusal` from `kNone` to `kRecording` alone writes the three fields, then publishes them
        // as `kRecorded`; the report moves it to `kReported` once. Later refusals record nothing.
        enum Refusal : uint32_t
        {
            kNone,
            kRecording,
            kRecorded,
            kReported,
        };
        std::atomic<uint32_t> m_refusal = kNone;
        uint32_t m_refusalSlot = 0;
        uint32_t m_refusalNumber = 0;
        std::string m_refusalError;
    };

    // Precise keeps each timestamp at its place among the encoder's commands; at an encoder's start and end, and after
    // the fragment stage inside a pass, it adds no encoder.
    constexpr MTL4::TimestampGranularity kEncoderGranularity = MTL4::TimestampGranularityPrecise;

    // One atomic add; the heap is created on its first claim. Free-threaded across lists, lock-free.
    uint32_t claimPair(CommandListImpl& list);

    uint32_t popPairEnd(CommandListImpl& list);

    // The command buffer takes it only between encoders. Opens an autorelease pool, because Metal autoreleases inside
    // the write.
    void writeBetweenEncoders(CommandListImpl& list, uint32_t index);

    // After the fragment stage of the work recorded before it; while the encoder holds no draw, after the vertex stage
    // at its start, because Metal may never write a fragment-stage timestamp on an encoder that draws nothing. The
    // encoder's write autoreleases nothing, so it opens no pool.
    void writeInPass(CommandListImpl& list, MTL4::RenderCommandEncoder* encoder, uint32_t index);

    // The kernels of counted draws that ran ahead of the pass are outside the pair: their encoder's opening barrier
    // waits for nothing and may overlap earlier work. The write follows the vertex stage, the encoder's first, so it
    // waits for none of the pass's work.
    void writePassBegin(CommandListImpl& list, MTL4::RenderCommandEncoder* encoder);

    void scopeTimes(void* context, uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report);
} // namespace rojoRHI::metal4
