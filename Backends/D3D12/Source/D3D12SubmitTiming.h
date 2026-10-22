//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12SubmitTiming.h
/// @brief Declares the state of the submission timing seam on a queue and the hooks acquisition, `end` and submission call.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"
#include "SubmitTiming.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace rojoRHI {
    struct CommandListImpl;
    struct QueueImpl;
} // namespace rojoRHI

namespace rojoRHI::d3d12 {
    /// A submission whose report waits for `drainSubmitTiming`.
    struct PendingSubmission
    {
        uint64_t serial = 0;    ///< The submission's serial on its queue.
        uint64_t startPair = 0; ///< The query pair of the submission's first list.
        uint64_t endPair = 0;   ///< The query pair of the submission's last list.
        bool timed = false;     ///< Both lists wrote their timestamps into this attachment's heap.
    };

    /// One attachment of `setSubmitTiming` to a queue, made whole at attach so neither a list nor a submission allocates.
    /// Each list acquired while it is attached claims a pair of queries, numbered from 0 and written at `pair %
    /// kPairs`; the pair's even query is the list's first timestamp and its odd one its last. A submission takes the next
    /// serial and, while the ring has room, a pending entry; once the ring is full the submissions up to the next drain
    /// report `failed`. A pair is overwritten once `kPairs` later pairs were claimed, which a drain detects.
    struct SubmitTimingState
    {
        static constexpr uint32_t kPendingSubmissions = 1024;                                     ///< The submissions the ring holds between two drains.
        static constexpr uint32_t kListsPerSubmission = 4;                                        ///< The lists per submission the queries are sized for.
        static constexpr uint64_t kPairs = uint64_t{ kPendingSubmissions } * kListsPerSubmission; ///< 4096 pairs.

        QueueType queue = QueueType::Graphics;                           ///< The queue's type, which each report names.
        SubmitTimingCallback callback = nullptr;                         ///< The callback each report calls.
        void* user = nullptr;                                            ///< The callback's first argument.
        Com<ID3D12QueryHeap> queries;                                    ///< `2 * kPairs` timestamp queries; null when the queue writes none.
        Com<ID3D12Resource> readback;                                    ///< A `READBACK` buffer of `2 * kPairs` ticks, mapped while it lives.
        const uint64_t* ticks = nullptr;                                 ///< The mapped readback buffer.
        Com<ID3D12Fence> fence;                                          ///< Signalled once by each drain that has reports to make.
        double gpuTicksPerSecond = 0.0;                                  ///< The queue's `GetTimestampFrequency` at attach.
        double cpuTicksPerSecond = 0.0;                                  ///< `QueryPerformanceFrequency` at attach.
        uint64_t gpuCalibration = 0;                                     ///< The GPU timestamp of `GetClockCalibration` at attach.
        uint64_t cpuCalibration = 0;                                     ///< The CPU timestamp of the same call, a `QueryPerformanceCounter` value.
        std::atomic<uint64_t> claimed = 0;                               ///< The pairs lists have claimed; acquisition is free-threaded.
        uint64_t signalled = 0;                                          ///< The fence value signalled last.
        uint64_t serial = 0;                                             ///< The serial given last.
        uint64_t overflowFrom = 0;                                       ///< The first serial the full ring refused since the last drain; 0 for none.
        uint32_t head = 0;                                               ///< The ring's oldest entry.
        uint32_t count = 0;                                              ///< The ring's entries.
        std::array<PendingSubmission, kPendingSubmissions> pending = {}; ///< The ring.
    };

    /// Called by `Device::acquireCommandList` after the native list's `Reset`: when the queue of the list's type is attached
    /// and writes timestamps, claims a pair for the list and records the `EndQuery` of its first timestamp; otherwise marks
    /// the list as writing none. Detached, it costs one relaxed atomic load. Counts nothing.
    void beginSubmitTiming(CommandListImpl& list);

    /// Called by `CommandList::end` before the list's own resolves and `Close`: when the list wrote its first timestamp,
    /// records the `EndQuery` of its last and one `ResolveQueryData` of both. Counts nothing.
    void endSubmitTiming(CommandListImpl& list);

    /// Called by `Queue::submit` after the `ExecuteCommandLists` of `lists`, which is not empty: when `queue` is attached,
    /// records the submission for its report, making no native call. Detached, it costs one relaxed atomic load.
    void noteSubmitTiming(QueueImpl& queue, std::span<const CommandList> lists);
} // namespace rojoRHI::d3d12
