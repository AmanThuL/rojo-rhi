//----------------------------------------------------------------------------------------------------------------------
/// @file NullQueue.cpp
/// @brief Implements the public queue entries for the Null backend: a submission completes during the call.
//----------------------------------------------------------------------------------------------------------------------

#include "NullDevice.h"

#include <rojoRHI/Queue.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Fences.h"
#include "Core/Sparse.h"
#include "Core/Swapchains.h"
#include "Core/Tracker/References.h"
#include "Core/Tracker/Tracker.h"

namespace rojoRHI {
    //==================================================================================================================
    QueueImpl& QueueImpl::of(Queue queue)
    {
        core::checkObject(queue.m_impl, "Queue");
        return *queue.m_impl;
    }

    //==================================================================================================================
    Queue QueueImpl::wrap(QueueImpl* impl)
    {
        Queue queue;
        queue.m_impl = impl;
        return queue;
    }

    //==================================================================================================================
    void Queue::submit(std::span<const CommandList> lists, std::span<const FenceValue> wait, std::span<const FenceValue> signal)
    {
        const QueueImpl& queue = QueueImpl::of(*this);
        core::DeviceCore& core = queue.device->core;
        core::submitLists(core, queue.type, lists);
        {
            // A validating device judges the submission under its tracker's lock, held until the signals are published, so a
            // submission on another queue sees the tracker state, the fence values and their clocks move together.
            const core::TrackerLock tracked = core::lockTracker(core);
            core::checkSubmit(core, wait, signal);
            core::replayLists(core, queue.type, lists, wait, signal, tracked);
            // The Null queue executes nothing, so every signal is reached before the call returns.
            core::recordSignals(core, signal, core::SignalCompletion::Immediate);
        }
        // The objects the lists name stay counted until replay has read them; the counts go outside the tracker's lock.
        core::releaseSubmitted(lists);
    }

    //==================================================================================================================
    void Queue::present(Swapchain swapchain)
    {
        const QueueImpl& queue = QueueImpl::of(*this);
        core::presentImage(queue.device->core, queue.type, SwapchainImpl::of(swapchain).core);
    }

    //==================================================================================================================
    void Queue::mapTiles(std::span<const TileMapping> mappings)
    {
        const QueueImpl& queue = QueueImpl::of(*this);
        core::mapTiles(queue.device->core, queue.type, mappings);
    }
} // namespace rojoRHI
