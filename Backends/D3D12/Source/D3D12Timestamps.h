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
    /// The device-wide timestamp state: each queue's tick frequency, read at creation and fixed for the device's life, and
    /// which retired frame last warned of untimed scopes. Only the scope-timing hook reads and writes it after creation.
    class Timestamps
    {
      public:
        /// Reads the timestamp frequency of each queue of `impl`, whose queues and features exist: the Copy queue's only when
        /// the device reports copy-queue timestamps. Counts nothing. Returns the error of a refused read.
        Result<void> create(DeviceImpl& impl);

        /// Releases what `create` made; nothing.
        void release();

        /// Returns the ticks per second of the timestamps the queue of `type` writes, or 0 when it writes none.
        double frequency(QueueType type) const;

        /// Fills `report[i]` with the GPU duration of `records[i]`, a scope of one queue's lists in the frame that retired from
        /// frame slot `slot` on `impl`, from the ticks each list's readback buffers hold. A record without a timestamp and an end
        /// before its begin give zero. The first queue of a retired frame that reports an untimed scope logs one warning that
        /// names the frame and the reason; the frame's other queues log none. The scope-timing hook calls it.
        void resolve(DeviceImpl& impl, uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report);

      private:
        std::array<double, 3> m_frequency = {}; ///< Ticks per second, indexed by `QueueType`; 0 for a queue that writes none.
        uint64_t m_warnedFrame = 0;             ///< The retired frame that last warned of untimed scopes; 0 for none.
    };

    /// The timestamp queries one command list records, its own so recording takes no lock: query heaps of `kHeapQueries`
    /// queries, each with a readback buffer it resolves into, made when a recording first needs one more and kept for the
    /// list's later recordings. A scope claims a pair of queries at index `next`; the pair's even index begins it and the odd
    /// one ends it. A pair never straddles two heaps.
    struct ListTimestamps
    {
        static constexpr uint32_t kHeapQueries = 256;     ///< The queries of one heap: 128 scopes.
        static constexpr uint32_t kMaxQueries = 1u << 16; ///< The queries of one recording; the scopes past them go untimed.
        static constexpr uint32_t kMaxOrdinal = 0xFFFF;   ///< The highest list ordinal a scope's timestamp encodes.

        /// One query heap and the readback buffer its queries resolve into.
        struct Heap
        {
            Com<ID3D12QueryHeap> queries;     ///< `kHeapQueries` timestamp queries, of the copy-queue type on a Copy list.
            Com<ID3D12Resource> readback;     ///< A `READBACK` buffer of `kHeapQueries` ticks of 8 bytes.
            const uint64_t* mapped = nullptr; ///< The readback buffer, mapped while the list lives.
        };

        std::vector<Heap> heaps;    ///< The list's heaps, in query order.
        uint32_t next = 0;          ///< The queries the recording has claimed.
        std::vector<uint32_t> open; ///< The first query of each open scope, innermost last; `core::kNoTimestamp` when untimed.

        /// Claims the next pair of queries for a scope of `list`, which owns these timestamps, and returns its first index.
        /// Returns `core::kNoTimestamp` when the recording has claimed `kMaxQueries` queries, when the list's ordinal exceeds
        /// `kMaxOrdinal`, or on a Copy list of a device without copy-queue timestamps. A pair that needs one more heap makes it
        /// and its readback buffer, named after the list, counting two `nativeCreateCalls`, one `gpuAllocations` and the
        /// mapping's one `nativeHostCalls`; a refused creation logs and aborts, in every build. Takes no lock.
        uint32_t claimPair(CommandListImpl& list);

        /// Forgets the previous recording's queries; called at each acquisition, after the native list's `Reset`. The heaps stay.
        void reset();

        /// Returns the duration in milliseconds between the ticks of queries `pair` and `pair + 1`, read from the readback
        /// buffer at `frequency` ticks per second, or zero when the end precedes the begin or the frequency is zero.
        double milliseconds(uint32_t pair, double frequency) const;
    };

    /// Records into `list` one `ResolveQueryData` for each query heap its recording used, over the queries it claimed there,
    /// into the heap's readback buffer, counted as one native call each; nothing for a recording without timed scopes.
    /// Called by `CommandList::end` before `Close`.
    void resolveTimestamps(CommandListImpl& list);

    /// The `scopeTimes` retire hook: fills the durations of the scopes the frame slot `slot` recorded on one queue. `context`
    /// is the `DeviceImpl`, whose `Timestamps::resolve` reads them.
    void scopeTimes(void* context, uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report);
} // namespace rojoRHI::d3d12
