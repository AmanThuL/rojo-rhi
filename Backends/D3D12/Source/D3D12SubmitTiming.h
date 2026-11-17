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
    struct PendingSubmission
    {
        uint64_t serial = 0;
        uint64_t startPair = 0;
        uint64_t endPair = 0;
        bool timed = false; // Both lists wrote their timestamps into this attachment's heap.
    };

    // Made whole at attach, so neither a list nor a submission allocates. Each list acquired while it is attached
    // claims a pair of queries, numbered from 0 and written at `pair % kPairs`: the even query is the list's first
    // timestamp, the odd one its last. A submission takes the next serial and, while the ring has room, a pending
    // entry; once the ring is full the submissions up to the next drain report `failed`. A pair is overwritten once
    // `kPairs` later pairs were claimed, which a drain detects.
    struct SubmitTimingState
    {
        static constexpr uint32_t kPendingSubmissions = 1024; // Between two drains.
        static constexpr uint32_t kListsPerSubmission = 4;
        static constexpr uint64_t kPairs = uint64_t{ kPendingSubmissions } * kListsPerSubmission;

        QueueType queue = QueueType::Graphics;
        SubmitTimingCallback callback = nullptr;
        void* user = nullptr;
        Com<ID3D12QueryHeap> queries; // `2 * kPairs` timestamp queries; null when the queue writes none.
        Com<ID3D12Resource> readback; // Mapped while it lives.
        const uint64_t* ticks = nullptr;
        Com<ID3D12Fence> fence; // Signalled once by each drain that has reports to make.
        double gpuTicksPerSecond = 0.0;
        double cpuTicksPerSecond = 0.0;
        uint64_t gpuCalibration = 0;       // From `GetClockCalibration` at attach.
        uint64_t cpuCalibration = 0;       // The CPU side of the same call, a `QueryPerformanceCounter` value.
        std::atomic<uint64_t> claimed = 0; // Acquisition is free-threaded.
        uint64_t signalled = 0;
        uint64_t serial = 0;
        uint64_t overflowFrom = 0; // The first serial the full ring refused since the last drain; 0 for none.
        uint32_t head = 0;
        uint32_t count = 0;
        std::array<PendingSubmission, kPendingSubmissions> pending = {};
    };

    // After the native list's `Reset`: when the queue of the list's type is attached and writes timestamps, claims a
    // pair for the list and records the `EndQuery` of its first timestamp. Detached, it costs one relaxed atomic load.
    // Counts nothing.
    void beginSubmitTiming(CommandListImpl& list);

    // Before the list's own resolves and `Close`: when the list wrote its first timestamp, records the `EndQuery` of
    // its last and one `ResolveQueryData` of both. Counts nothing.
    void endSubmitTiming(CommandListImpl& list);

    // After the `ExecuteCommandLists` of `lists`, which is not empty; makes no native call. Detached, it costs one
    // relaxed atomic load.
    void noteSubmitTiming(QueueImpl& queue, std::span<const CommandList> lists);
} // namespace rojoRHI::d3d12
