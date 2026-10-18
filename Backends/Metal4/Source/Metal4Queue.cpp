//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Queue.cpp
/// @brief Implements the public queue entries for the Metal 4 backend: submissions wait, commit the lists and signal on the native queue.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Fences.h"
#include "Core/Tracker/References.h"
#include "Core/Tracker/Tracker.h"
#include "Metal4Common.h"
#include "Metal4Sparse.h"

#include <Metal/Metal.hpp>

#include <atomic>
#include <span>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Commits the buffers in `queue.commitBuffers` in one native call. With a commit timing callback set, the commit carries
        /// feedback that reports the GPU span of this commit; commit options are not native calls and count nothing.
        void commit(QueueImpl& queue)
        {
            if (queue.commitCallback == nullptr)
            {
                queue.native->commit(queue.commitBuffers.data(), queue.commitBuffers.size());
                return;
            }
            const metal4::CommitTimingCallback callback = queue.commitCallback;
            void* const user = queue.commitUser;
            const QueueType type = queue.type;
            const uint64_t serial = ++queue.commitSerial;
            MTL4::CommitOptions* options = MTL4::CommitOptions::alloc()->init();
            options->addFeedbackHandler([callback, user, type, serial](MTL4::CommitFeedback* feedback) {
                // Metal's notification thread holds no autorelease pool.
                const metal4::AutoreleasePool handlerPool;
                const metal4::CommitTime time{ type, serial, feedback->GPUStartTime(), feedback->GPUEndTime(), feedback->error() != nullptr };
                callback(user, time);
            });
            queue.native->commit(queue.commitBuffers.data(), queue.commitBuffers.size(), options);
            options->release();
        }
    } // namespace

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
        QueueImpl& queue = QueueImpl::of(*this);
        DeviceImpl& device = *queue.device;
        core::DeviceCore& core = device.core;
        core::submitLists(core, queue.type, lists);
        {
            // A validating device judges the submission under its tracker's lock, held until the signals are recorded, so a
            // submission on another queue sees the tracker state and the submitted fence values move together.
            const core::TrackerLock tracked = core::lockTracker(core);
            core::checkSubmit(core, wait, signal);
            core::replayLists(core, queue.type, lists, wait, signal, tracked);
            // A clean residency set is seen without a lock; a dirty one is committed under its mutex, which serialises the
            // queues that submit concurrently.
            if (device.residency.commitIfDirty())
            {
                core::DeviceCounts::add(core.counts().nativeHostCalls);
            }
            // The signals are recorded before the queue can reach them, so a value completedValue observes is never above
            // the greatest value submitted for signal (VU-rojo-queue-0008 reads both).
            core::recordSignals(core, signal, core::SignalCompletion::Later);
            // What swapchain creation and retirement left for the graphics queue is applied before any command buffer commits.
            metal4::flushPresentation(queue);
            // checkSubmit has accepted every fence, so their events are read without a lock.
            for (const FenceValue& entry : wait)
            {
                queue.native->wait(static_cast<const MTL::SharedEvent*>(core.pool<Fence>().native(entry.fence)), entry.value);
            }
            if (!lists.empty())
            {
                // One commit of every list's command buffer, in span order, from a vector only this queue's submissions touch.
                queue.commitBuffers.clear();
                for (const CommandList& list : lists)
                {
                    queue.commitBuffers.push_back(CommandListImpl::of(list).state.commandBuffer);
                }
                commit(queue);
                core::DeviceCounts::add(core.counts().nativeSubmitCalls);
            }
            for (const FenceValue& entry : signal)
            {
                queue.native->signalEvent(static_cast<const MTL::SharedEvent*>(core.pool<Fence>().native(entry.fence)), entry.value);
            }
            if (!wait.empty() || !signal.empty())
            {
                // A destroyed fence's event is kept until the queue passes this point; the signal is a native queue call.
                const uint64_t progress = queue.progressValue.load(std::memory_order_relaxed) + 1;
                queue.progressValue.store(progress, std::memory_order_relaxed);
                queue.native->signalEvent(queue.progress, progress);
                core::DeviceCounts::add(core.counts().nativeSubmitCalls);
            }
        }
        // The objects the lists name stay counted until replay has read them; the counts go outside the tracker's lock.
        core::releaseSubmitted(lists);
    }

    //==================================================================================================================
    void Queue::present(Swapchain swapchain)
    {
        QueueImpl& queue = QueueImpl::of(*this);
        metal4::presentSwapchain(queue, SwapchainImpl::of(swapchain));
    }

    //==================================================================================================================
    void Queue::mapTiles(std::span<const TileMapping> mappings)
    {
        metal4::mapTiles(QueueImpl::of(*this), mappings);
    }
} // namespace rojoRHI
