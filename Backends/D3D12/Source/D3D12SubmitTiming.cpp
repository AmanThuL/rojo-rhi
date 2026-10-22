//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12SubmitTiming.cpp
/// @brief Implements the private seam that makes a queue's submissions report their GPU time when the caller drains them.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12SubmitTiming.h"

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12ListState.h"
#include "SubmitTiming.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace rojoRHI::d3d12 {
    namespace {
        /// The name of each queue type in native names, indexed by `QueueType`.
        constexpr std::array<std::string_view, 3> kTypeNames = { "graphics", "compute", "copy" };

        /// How long a drain waits for one submission before it reports it and every later one `failed`: sixty seconds, the
        /// benchmark harness's frame wait.
        constexpr uint64_t kDrainTimeoutNs = 60'000'000'000;

        //==============================================================================================================
        /// Returns the first query of `pair`, the even one, in the attachment's heap.
        UINT firstQuery(uint64_t pair)
        {
            return static_cast<UINT>(pair % SubmitTimingState::kPairs) * 2;
        }

        //==============================================================================================================
        /// Returns `ticks`, a timestamp of the attachment's queue, in seconds of the CPU's `QueryPerformanceCounter`: the CPU
        /// time of the calibration pair plus the GPU ticks since its GPU time.
        double cpuSeconds(const SubmitTimingState& timing, uint64_t ticks)
        {
            const auto sinceCalibration = static_cast<int64_t>(ticks - timing.gpuCalibration);
            return static_cast<double>(timing.cpuCalibration) / timing.cpuTicksPerSecond + static_cast<double>(sinceCalibration) / timing.gpuTicksPerSecond;
        }

        //==============================================================================================================
        /// Makes the query heap, the mapped readback buffer and the fence of an attachment of `queue`, named after the queue,
        /// and reads the queue's calibration pair. A Copy queue on a device without copy-queue timestamps gets no heap and no
        /// buffer. A refused call logs and aborts, in every build. Counts nothing.
        std::unique_ptr<SubmitTimingState> createState(QueueImpl& queue, SubmitTimingCallback callback, void* user)
        {
            DeviceImpl& impl = *queue.device;
            auto timing = std::make_unique<SubmitTimingState>();
            timing->queue = queue.type;
            timing->callback = callback;
            timing->user = user;
            const std::string name = std::format("rojorhi.queue.{}.submitTiming", kTypeNames[static_cast<size_t>(queue.type)]);
            requireSucceeded(
                impl,
                impl.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(timing->fence.put())),
                std::format("ID3D12Device::CreateFence for {}", name)
            );
            setName(timing->fence.get(), name);
            if (queue.type == QueueType::Copy && !impl.features.copyQueueTimestamps)
            {
                return timing;
            }
            D3D12_QUERY_HEAP_DESC queries = {};
            queries.Type = queue.type == QueueType::Copy ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            queries.Count = static_cast<UINT>(SubmitTimingState::kPairs * 2);
            queries.NodeMask = 0;
            requireSucceeded(
                impl, impl.device()->CreateQueryHeap(&queries, IID_PPV_ARGS(timing->queries.put())), std::format("ID3D12Device::CreateQueryHeap for {}", name)
            );
            setName(timing->queries.get(), name + ".queries");
            D3D12_HEAP_PROPERTIES properties = {};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC1 desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = SubmitTimingState::kPairs * 2 * sizeof(uint64_t);
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            requireSucceeded(
                impl,
                impl.device()->CreateCommittedResource3(
                    &properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(timing->readback.put())
                ),
                std::format("ID3D12Device10::CreateCommittedResource3 of {}.readback", name)
            );
            setName(timing->readback.get(), name + ".readback");
            // The buffer stays mapped: the CPU reads a submission's ticks only after the seam's fence shows its lists complete.
            void* mapped = nullptr;
            requireSucceeded(impl, timing->readback->Map(0, nullptr, &mapped), std::format("ID3D12Resource::Map of {}.readback", name));
            timing->ticks = static_cast<const uint64_t*>(mapped);
            UINT64 frequency = 0;
            requireSucceeded(impl, queue.native->GetTimestampFrequency(&frequency), std::format("ID3D12CommandQueue::GetTimestampFrequency for {}", name));
            UINT64 gpuTimestamp = 0;
            UINT64 cpuTimestamp = 0;
            requireSucceeded(
                impl, queue.native->GetClockCalibration(&gpuTimestamp, &cpuTimestamp), std::format("ID3D12CommandQueue::GetClockCalibration for {}", name)
            );
            // QueryPerformanceFrequency cannot fail on Windows XP or later.
            LARGE_INTEGER cpuFrequency = {};
            QueryPerformanceFrequency(&cpuFrequency);
            timing->gpuTicksPerSecond = static_cast<double>(frequency);
            timing->cpuTicksPerSecond = static_cast<double>(cpuFrequency.QuadPart);
            timing->gpuCalibration = gpuTimestamp;
            timing->cpuCalibration = cpuTimestamp;
            return timing;
        }

        //==============================================================================================================
        /// Makes the report of `entry` of `timing` through its callback: its two timestamps in seconds when `reached`, the
        /// seam's fence having shown every pending submission complete, its lists both wrote into this attachment and no
        /// later list has claimed their pairs again; otherwise `failed` with zero times.
        void report(const SubmitTimingState& timing, const PendingSubmission& entry, bool reached)
        {
            SubmitTime time{ .queue = timing.queue, .serial = entry.serial, .gpuStartSeconds = 0.0, .gpuEndSeconds = 0.0, .failed = true };
            if (reached && entry.timed)
            {
                const uint64_t start = timing.ticks[firstQuery(entry.startPair)];
                const uint64_t end = timing.ticks[firstQuery(entry.endPair) + 1];
                // Read after the ticks: a pair claimed again before the read may have overwritten them, one claimed after cannot.
                const bool reclaimed = timing.claimed.load(std::memory_order_acquire) > std::min(entry.startPair, entry.endPair) + SubmitTimingState::kPairs;
                if (!reclaimed)
                {
                    time.gpuStartSeconds = cpuSeconds(timing, start);
                    time.gpuEndSeconds = cpuSeconds(timing, end);
                    time.failed = false;
                }
            }
            timing.callback(timing.user, time);
        }
    } // namespace

    //==================================================================================================================
    void setSubmitTiming(Queue queue, SubmitTimingCallback callback, void* user)
    {
        QueueImpl& impl = QueueImpl::of(queue);
        // The previous attachment stays allocated in `timings`: a list acquired while it was attached may still write into it.
        impl.timing.store(nullptr, std::memory_order_relaxed);
        if (callback == nullptr)
        {
            return;
        }
        impl.timings.push_back(createState(impl, callback, user));
        impl.timing.store(impl.timings.back().get(), std::memory_order_release);
    }

    //==================================================================================================================
    void drainSubmitTiming(Queue queue)
    {
        QueueImpl& impl = QueueImpl::of(queue);
        SubmitTimingState* timing = impl.timing.load(std::memory_order_acquire);
        if (timing == nullptr || (timing->count == 0 && timing->overflowFrom == 0))
        {
            return;
        }
        // One signal after every submission made so far: the queue runs its work in order, so once the fence holds the value
        // every pending submission has completed. A refused signal or a removed device makes every report `failed`.
        const uint64_t value = ++timing->signalled;
        bool reached = SUCCEEDED(impl.native->Signal(timing->fence.get(), value));
        if (reached)
        {
            const FenceWait wait = waitForFence(timing->fence.get(), value, kDrainTimeoutNs, "the submission timing fence");
            reached = wait.reached && wait.completed != UINT64_MAX;
        }
        while (timing->count > 0)
        {
            const PendingSubmission entry = timing->pending[timing->head];
            timing->head = (timing->head + 1) % SubmitTimingState::kPendingSubmissions;
            --timing->count;
            report(*timing, entry, reached);
        }
        // The submissions the full ring refused come after every entry it held, up to the last serial.
        for (uint64_t serial = std::exchange(timing->overflowFrom, 0); serial != 0 && serial <= timing->serial; ++serial)
        {
            report(*timing, PendingSubmission{ .serial = serial }, false);
        }
    }

    //==================================================================================================================
    void beginSubmitTiming(CommandListImpl& list)
    {
        ListState& state = list.state;
        SubmitTimingState* timing = deviceOf(list).queues[static_cast<size_t>(state.type)].timing.load(std::memory_order_relaxed);
        state.submitTiming = nullptr;
        if (timing == nullptr)
        {
            return;
        }
        // Pairs with the release store of the attach, so the heap the load found is seen whole.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!timing->queries)
        {
            return;
        }
        state.submitTiming = timing;
        state.submitPair = timing->claimed.fetch_add(1, std::memory_order_relaxed);
        state.list->EndQuery(timing->queries.get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery(state.submitPair));
    }

    //==================================================================================================================
    void endSubmitTiming(CommandListImpl& list)
    {
        const ListState& state = list.state;
        const SubmitTimingState* timing = state.submitTiming;
        if (timing == nullptr)
        {
            return;
        }
        const UINT first = firstQuery(state.submitPair);
        state.list->EndQuery(timing->queries.get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
        state.list->ResolveQueryData(timing->queries.get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2, timing->readback.get(), uint64_t{ first } * sizeof(uint64_t));
    }

    //==================================================================================================================
    void noteSubmitTiming(QueueImpl& queue, std::span<const CommandList> lists)
    {
        SubmitTimingState* timing = queue.timing.load(std::memory_order_relaxed);
        if (timing == nullptr)
        {
            return;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t serial = ++timing->serial;
        if (timing->count == SubmitTimingState::kPendingSubmissions)
        {
            if (timing->overflowFrom == 0)
            {
                timing->overflowFrom = serial;
            }
            return;
        }
        const ListState& first = CommandListImpl::of(lists.front()).state;
        const ListState& last = CommandListImpl::of(lists.back()).state;
        timing->pending[(timing->head + timing->count) % SubmitTimingState::kPendingSubmissions] = PendingSubmission{
            .serial = serial,
            .startPair = first.submitPair,
            .endPair = last.submitPair,
            .timed = first.submitTiming == timing && last.submitTiming == timing,
        };
        ++timing->count;
    }
} // namespace rojoRHI::d3d12
