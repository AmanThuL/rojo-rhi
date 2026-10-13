//----------------------------------------------------------------------------------------------------------------------
/// @file NullQueue.cpp
/// @brief Implements the public queue entries for the Null backend: a submission completes during the call.
//----------------------------------------------------------------------------------------------------------------------

#include "NullDevice.h"

#include <rojoRHI/Queue.h>

#include "Core/DeviceCore.h"
#include "Core/Fences.h"

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
        core::DeviceCore& core = QueueImpl::of(*this).device->core;
        core::checkSubmit(core, lists, wait, signal);
        // The Null queue executes nothing, so every signal is reached before the call returns.
        core::recordSignals(core, signal, core::SignalCompletion::Immediate);
    }
} // namespace rojoRHI
