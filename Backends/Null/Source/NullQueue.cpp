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
        core::checkSubmit(core, wait, signal);
        // The Null queue executes nothing, so every signal is reached before the call returns.
        core::recordSignals(core, signal, core::SignalCompletion::Immediate);
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
        core::mapTiles(QueueImpl::of(*this).device->core, mappings);
    }
} // namespace rojoRHI
