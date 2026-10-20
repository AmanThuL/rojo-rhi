//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Queue.cpp
/// @brief Implements submission on the D3D12 queues, which waits and signals fences; presentation and tile mapping abort.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Fences.h"
#include "Core/Tracker/References.h"
#include "Core/Tracker/Tracker.h"
#include "D3D12Common.h"
#include "D3D12Fences.h"

#include <atomic>
#include <cstdint>
#include <span>

namespace rojoRHI {
    //==================================================================================================================
    void Queue::submit(std::span<const CommandList> lists, std::span<const FenceValue> wait, std::span<const FenceValue> signal)
    {
        QueueImpl& queue = QueueImpl::of(*this);
        DeviceImpl& device = *queue.device;
        core::DeviceCore& core = device.core;
        core::submitLists(core, queue.type, lists);
        {
            // A validating device judges the submission under its tracker's lock, held until the last native call, so a
            // submission on another queue sees the tracker state and the submitted fence values move together.
            const core::TrackerLock tracked = core::lockTracker(core);
            // It also counts one native queue call for each wait and signal below.
            core::checkSubmit(core, wait, signal);
            core::replayLists(core, queue.type, lists, wait, signal, tracked);
            // The signals are recorded before the queue can reach them, so a value completedValue observes is never above
            // the greatest value submitted for signal (VU-rojo-queue-0008 reads both).
            core::recordSignals(core, signal, core::SignalCompletion::Later);
            // checkSubmit has accepted every fence, so their native fences are read without a lock.
            for (const FenceValue& entry : wait)
            {
                auto* fence = static_cast<ID3D12Fence*>(core.pool<Fence>().native(entry.fence));
                d3d12::requireSucceeded(device, queue.native->Wait(fence, entry.value), "ID3D12CommandQueue::Wait in Queue::submit");
            }
            // A list encodes no native command, so nothing is executed between the waits and the signals.
            for (const FenceValue& entry : signal)
            {
                auto* fence = static_cast<ID3D12Fence*>(core.pool<Fence>().native(entry.fence));
                d3d12::requireSucceeded(device, queue.native->Signal(fence, entry.value), "ID3D12CommandQueue::Signal in Queue::submit");
            }
            // A frame's retirement covers every graphics submission made before its end and, as the frame joins them through a
            // signal (VU-rojo-device-0006), the compute and copy submissions with lists and at most one signal. A fence-only
            // compute or copy submission joins no frame, and a join through one of several signals leaves the later signals
            // unordered, so after either the queue signals its progress: a destroyed fence's native fence is kept until the
            // queue passes this point.
            const bool joined = !lists.empty() && signal.size() <= 1;
            if (queue.type != QueueType::Graphics && !joined && (!wait.empty() || !signal.empty()))
            {
                const uint64_t progress = queue.progress.value.load(std::memory_order_relaxed) + 1;
                queue.progress.value.store(progress, std::memory_order_relaxed);
                d3d12::requireSucceeded(
                    device, queue.native->Signal(queue.progress.fence.get(), progress), "ID3D12CommandQueue::Signal of the progress fence in Queue::submit"
                );
                core::DeviceCounts::add(core.counts().nativeSubmitCalls);
            }
        }
        // The objects the lists name stay counted until replay has read them; the counts go outside the tracker's lock.
        core::releaseSubmitted(lists);
    }

    //==================================================================================================================
    void Queue::present(Swapchain)
    {
        d3d12::notImplemented("Queue::present");
    }

    //==================================================================================================================
    void Queue::mapTiles(std::span<const TileMapping>)
    {
        d3d12::notImplemented("Queue::mapTiles");
    }
} // namespace rojoRHI
