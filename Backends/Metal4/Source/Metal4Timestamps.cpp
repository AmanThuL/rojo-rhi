//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Timestamps.cpp
/// @brief Implements the counter heaps of a Metal 4 device (claims, creation, the frame wait's resolve, reuse) and a list's timestamp writes.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Timestamps.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/ScopeRecord.h"
#include "Metal4Common.h"
#include "Metal4Device.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace rojoRHI::metal4 {
    namespace {
        /// The entries of every heap of one frame slot.
        constexpr uint32_t kSlotEntries = Timestamps::kSlotHeaps * Timestamps::kHeapEntries;

        //==============================================================================================================
        /// Returns the frame slot `list` records into, which owns the timestamp pairs its scopes claim.
        uint32_t slotOf(const core::CommandListCore& list)
        {
            return static_cast<uint32_t>(list.frame % list.device->framesInFlight());
        }

        //==============================================================================================================
        /// Returns the device `list` records for: the retire hooks' context, fixed from its creation to its destruction.
        DeviceImpl& deviceOf(const core::CommandListCore& list)
        {
            return *static_cast<DeviceImpl*>(list.device->retireHooks().context);
        }
    } // namespace

    //==================================================================================================================
    void Timestamps::create(MTL::Device* device)
    {
        m_device = device;
        m_frequency = device->queryTimestampFrequency();
    }

    //==================================================================================================================
    uint32_t Timestamps::claim(uint32_t slot, core::DeviceCounts& counts)
    {
        Slot& state = m_slots[slot];
        // The relaxed add is the whole claim: each pair is one claim's alone, and the heap it lands in is published below.
        const uint32_t index = state.cursor.fetch_add(2, std::memory_order_relaxed);
        if (index >= kSlotEntries)
        {
            return core::kNoTimestamp;
        }
        const uint32_t number = index / kHeapEntries;
        if (state.heaps[number].load(std::memory_order_acquire) == nullptr && !createHeap(slot, number, counts))
        {
            return core::kNoTimestamp;
        }
        return index;
    }

    //==================================================================================================================
    bool Timestamps::createHeap(uint32_t slot, uint32_t number, core::DeviceCounts& counts)
    {
        Slot& state = m_slots[slot];
        if (state.refused[number].load(std::memory_order_relaxed))
        {
            return false;
        }
        // The label and an error are autoreleased; a heap is created once per slot and number, so the pool is not per scope.
        const AutoreleasePool pool;
        MTL4::CounterHeapDescriptor* descriptor = MTL4::CounterHeapDescriptor::alloc()->init();
        descriptor->setType(MTL4::CounterHeapTypeTimestamp);
        descriptor->setCount(kHeapEntries);
        NS::Error* error = nullptr;
        MTL4::CounterHeap* created = m_device->newCounterHeap(descriptor, &error);
        descriptor->release();
        if (created == nullptr)
        {
            // Another claim may have published the heap while Metal refused this one; its pairs are timed in that heap.
            if (state.heaps[number].load(std::memory_order_acquire) != nullptr)
            {
                return true;
            }
            // Metal limits the counter heaps per process or on the system. The refusal holds until the slot's reuse, so a
            // frame asks Metal at most once per heap; the scopes that need this heap go untimed.
            state.refused[number].store(true, std::memory_order_relaxed);
            if (!m_refusalLogged.exchange(true, std::memory_order_relaxed))
            {
                ROJORHI_LOG_WARN("Metal refused timestamp heap {} of frame slot {}, so scopes go untimed: {}", number, slot, describe(error));
            }
            return false;
        }
        created->setLabel(makeString(std::format("rojorhi.timestamps.{}.{}", slot, number)));
        MTL4::CounterHeap* expected = nullptr;
        if (!state.heaps[number].compare_exchange_strong(expected, created, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            // Another claim published its heap first; that one is kept.
            created->release();
            return true;
        }
        core::DeviceCounts::add(counts.nativeCreateCalls);
        return true;
    }

    //==================================================================================================================
    uint32_t Timestamps::used(const Slot& slot) const
    {
        return std::min(slot.cursor.load(std::memory_order_relaxed), kSlotEntries);
    }

    //==================================================================================================================
    double Timestamps::milliseconds(const Slot& slot, uint32_t index) const
    {
        const NS::Data* data = slot.resolved[index / kHeapEntries];
        const uint32_t entry = index % kHeapEntries;
        if (data == nullptr || m_frequency == 0 || (entry + 2) * sizeof(MTL4::TimestampHeapEntry) > data->length())
        {
            return 0.0;
        }
        const auto* entries = static_cast<const MTL4::TimestampHeapEntry*>(data->bytes());
        const uint64_t begin = entries[entry].timestamp;
        const uint64_t end = entries[entry + 1].timestamp;
        // Metal resolves an entry no timestamp wrote, in a new heap or after an invalidation, to zero; the error value is
        // what Metal documents for one, and is taken as unwritten too.
        if (begin == 0 || end == 0 || begin == MTL::CounterErrorValue || end == MTL::CounterErrorValue || end < begin)
        {
            return 0.0;
        }
        return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(m_frequency);
    }

    //==================================================================================================================
    void Timestamps::resolve(uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report, core::DeviceCounts& counts)
    {
        Slot& state = m_slots[slot];
        const AutoreleasePool pool;
        if (!state.resolvedFrame)
        {
            // The frame wait proved every entry claimed for the retiring frame written, and nothing claims in this slot until
            // its reuse, so the cursor holds still.
            const uint32_t entries = used(state);
            for (uint32_t number = 0; number * kHeapEntries < entries; ++number)
            {
                MTL4::CounterHeap* heap = state.heaps[number].load(std::memory_order_acquire);
                if (heap == nullptr)
                {
                    continue;
                }
                const uint32_t count = std::min(entries - number * kHeapEntries, kHeapEntries);
                NS::Data* data = heap->resolveCounterRange(NS::Range::Make(0, count));
                core::DeviceCounts::add(counts.nativeHostCalls);
                state.resolved[number] = data != nullptr ? data->retain() : nullptr;
            }
            state.resolvedFrame = true;
            state.frame = report.empty() ? 0 : report.front().frame;
        }
        for (size_t i = 0; i < records.size(); ++i)
        {
            if (records[i].timestamp == core::kNoTimestamp)
            {
                ++state.untimed;
                continue;
            }
            report[i].gpuMilliseconds = milliseconds(state, records[i].timestamp);
        }
    }

    //==================================================================================================================
    void Timestamps::reuse(uint32_t slot, core::DeviceCounts& counts)
    {
        Slot& state = m_slots[slot];
        const AutoreleasePool pool;
        const uint32_t entries = used(state);
        // An invalidated entry reads as unwritten until a timestamp overwrites it, so an entry the next frame leaves
        // unwritten is never read as a time.
        for (uint32_t number = 0; number * kHeapEntries < entries; ++number)
        {
            MTL4::CounterHeap* heap = state.heaps[number].load(std::memory_order_acquire);
            if (heap != nullptr)
            {
                heap->invalidateCounterRange(NS::Range::Make(0, std::min(entries - number * kHeapEntries, kHeapEntries)));
                core::DeviceCounts::add(counts.nativeHostCalls);
            }
        }
        for (NS::Data*& data : state.resolved)
        {
            if (data != nullptr)
            {
                data->release();
                data = nullptr;
            }
        }
        if (state.untimed > 0)
        {
            // Scopes go untimed past the slot's capacity, for a heap Metal refused, and wherever no timestamp is written for
            // a scope; only the first two are known here.
            bool refusedUsed = false;
            for (uint32_t number = 0; number * kHeapEntries < entries; ++number)
            {
                refusedUsed = refusedUsed || state.refused[number].load(std::memory_order_relaxed);
            }
            const bool beyondCapacity = state.cursor.load(std::memory_order_relaxed) > kSlotEntries;
            ROJORHI_LOG_WARN(
                "frame {}: {} scope(s) report no GPU time{}{}",
                state.frame,
                state.untimed,
                beyondCapacity ? std::format("; the frame claimed more than the {} timestamp pairs of a frame slot", kSlotEntries / 2) : std::string(),
                refusedUsed ? "; Metal refused a counter heap the frame needed" : ""
            );
        }
        state.cursor.store(0, std::memory_order_relaxed);
        // Another process may have released heaps by the next frame, so a refused heap is asked for again.
        for (std::atomic<bool>& refused : state.refused)
        {
            refused.store(false, std::memory_order_relaxed);
        }
        state.resolvedFrame = false;
        state.untimed = 0;
    }

    //==================================================================================================================
    void Timestamps::release()
    {
        for (Slot& state : m_slots)
        {
            for (NS::Data*& data : state.resolved)
            {
                if (data != nullptr)
                {
                    data->release();
                    data = nullptr;
                }
            }
            for (std::atomic<MTL4::CounterHeap*>& heap : state.heaps)
            {
                if (MTL4::CounterHeap* owned = heap.exchange(nullptr, std::memory_order_acq_rel); owned != nullptr)
                {
                    owned->release();
                }
            }
        }
    }

    //==================================================================================================================
    void scopeTimes(void* context, uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report)
    {
        DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
        impl.timestamps.resolve(slot, records, report, impl.core.counts());
    }

    //==================================================================================================================
    uint32_t claimPair(CommandListImpl& list)
    {
        DeviceImpl& device = deviceOf(list.core);
        const uint32_t pair = device.timestamps.claim(slotOf(list.core), device.core.counts());
        list.core.scopeLog.back().timestamp = pair;
        list.state.scopeTimestamps.push_back(pair);
        return pair;
    }

    //==================================================================================================================
    uint32_t popPairEnd(CommandListImpl& list)
    {
        const uint32_t pair = list.state.scopeTimestamps.back();
        list.state.scopeTimestamps.pop_back();
        return pair == core::kNoTimestamp ? core::kNoTimestamp : pair + 1;
    }

    //==================================================================================================================
    void writeBetweenEncoders(CommandListImpl& list, uint32_t index)
    {
        if (index == core::kNoTimestamp)
        {
            return;
        }
        const Timestamps& timestamps = deviceOf(list.core).timestamps;
        const CommandPool pool(list.state.commandPools);
        list.state.commandBuffer->writeTimestampIntoHeap(timestamps.heapFor(slotOf(list.core), index), index % Timestamps::kHeapEntries);
        core::noteNativeCall(list.core);
    }

    //==================================================================================================================
    void writeInPass(CommandListImpl& list, MTL4::RenderCommandEncoder* encoder, uint32_t index)
    {
        if (index == core::kNoTimestamp)
        {
            return;
        }
        const Timestamps& timestamps = deviceOf(list.core).timestamps;
        // Unlike the command buffer's write, the encoder's autoreleases nothing, with or without the validation layer, so it
        // opens no pool.
        encoder->writeTimestamp(kInPassGranularity, MTL::RenderStageFragment, timestamps.heapFor(slotOf(list.core), index), index % Timestamps::kHeapEntries);
        core::noteNativeCall(list.core);
    }
} // namespace rojoRHI::metal4
