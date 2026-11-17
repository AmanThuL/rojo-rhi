//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Timestamps.h
/// @brief Declares the D3D12 device's timestamp frequencies, each command list's timestamp queries and the scope-timing hook.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/ScopeRecord.h"
#include "D3D12Common.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace rojoRHI {
    struct CommandListImpl;
    struct DeviceImpl;
} // namespace rojoRHI

namespace rojoRHI::d3d12 {
    // Each queue's tick frequency is read at creation and fixed for the device's life. Only the scope-timing hook reads
    // and writes this state after creation.
    class Timestamps
    {
      public:
        // Reads each queue's tick frequency, the Copy queue's only when the device reports copy-queue timestamps.
        // Counts nothing.
        Result<void> create(DeviceImpl& impl);

        void release();

        // 0 when the queue writes none.
        double frequency(QueueType type) const;

        // A record without a timestamp, or an end before its begin, gives zero. The first queue of a retired frame that
        // reports an untimed scope logs one warning naming the frame and the reason; the frame's other queues log none.
        void resolve(DeviceImpl& impl, uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report);

      private:
        std::array<double, 3> m_frequency = {}; // Indexed by `QueueType`; 0 for a queue that writes none.
        uint64_t m_warnedFrame = 0;
    };

    // The list's own, so recording takes no lock. Heaps of `kHeapQueries` queries, each with a readback buffer it
    // resolves into, are made when a recording first needs one more and kept for later recordings. A scope's pair
    // begins at the even index and ends at the odd one; a pair never straddles two heaps.
    struct ListTimestamps
    {
        static constexpr uint32_t kHeapQueries = 256;     // 128 scopes.
        static constexpr uint32_t kMaxQueries = 1u << 16; // Per recording; the scopes past them go untimed.
        static constexpr uint32_t kMaxOrdinal = 0xFFFF;   // The highest list ordinal a scope's timestamp encodes.

        struct Heap
        {
            Com<ID3D12QueryHeap> queries; // Of the copy-queue type on a Copy list.
            Com<ID3D12Resource> readback;
            const uint64_t* mapped = nullptr; // Mapped while the list lives.
        };

        // Decided at the recording's first claim: the device, the list's ordinal and its queue type do not change while
        // it records.
        enum class Gate : uint8_t
        {
            Unknown,
            Open,
            Closed,
        };

        std::vector<Heap> heaps;
        uint32_t next = 0;
        Gate gate = Gate::Unknown;  // Reset at each acquisition.
        std::vector<uint32_t> open; // Innermost last; `core::kNoTimestamp` for an untimed scope.

        // `core::kNoTimestamp` on a device created without `DeviceDesc::scopeTimings`, once the recording has claimed
        // `kMaxQueries` queries, when the list's ordinal exceeds `kMaxOrdinal`, or on a Copy list of a device without
        // copy-queue timestamps. A pair that needs one more heap makes it and its readback buffer, counting two
        // `nativeCreateCalls`, one `gpuAllocations` and the mapping's one `nativeHostCalls`; a refused creation logs
        // and aborts, in every build. Takes no lock.
        uint32_t claimPair(CommandListImpl& list);

        // At each acquisition, after the native list's `Reset`. The heaps stay.
        void reset();

        // Zero when the end precedes the begin or the frequency is zero.
        double milliseconds(uint32_t pair, double frequency) const;
    };

    // One `ResolveQueryData` per query heap the recording used, counted as one native call each; nothing without timed
    // scopes. `CommandList::end` calls it before `Close`.
    void resolveTimestamps(CommandListImpl& list);

    // `context` is the DeviceImpl.
    void scopeTimes(void* context, uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report);
} // namespace rojoRHI::d3d12
