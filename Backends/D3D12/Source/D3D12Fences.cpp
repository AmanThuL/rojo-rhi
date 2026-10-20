//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Fences.cpp
/// @brief Implements the D3D12 queues, fences and frame pacing: each fence is an `ID3D12Fence`, and so is the frame fence.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Fences.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Fences.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Device.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace rojoRHI {
    namespace {
        /// The label of each caller-visible queue, indexed by `QueueType`.
        constexpr std::array<std::string_view, 3> kQueueLabels = { "rojorhi.queue.graphics", "rojorhi.queue.compute", "rojorhi.queue.copy" };

        /// The native list type of each queue, indexed by `QueueType`.
        constexpr std::array<D3D12_COMMAND_LIST_TYPE, 3> kQueueTypes = { D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                         D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                                         D3D12_COMMAND_LIST_TYPE_COPY };

        //==============================================================================================================
        /// Reports the removal of `impl`'s device, found by `entry`, then logs and aborts, in every build, when the device was
        /// removed; does nothing otherwise. A removed device's fences report `UINT64_MAX`, a value no entry may pass on as
        /// reached.
        void abortIfRemoved(DeviceImpl& impl, std::string_view entry)
        {
            if (const HRESULT reason = impl.device()->GetDeviceRemovedReason(); reason != S_OK)
            {
                d3d12::reportRemoval(impl, entry);
                ROJORHI_LOG_ERROR("{} found the D3D12 device removed with reason 0x{:08X}", entry, d3d12::hresultBits(reason));
                std::abort();
            }
        }

        //==============================================================================================================
        /// Blocks until `fence` reaches `value` or `timeoutNs` nanoseconds pass and returns whether it reached the value; a zero
        /// timeout only polls. A value already reached costs one counted `GetCompletedValue`; a wait goes through
        /// `d3d12::waitForFence`, which reads the fence once more and arms one event, two more counted host calls. Before a
        /// reached value is returned, a device found removed is reported and aborts (`entry` names the caller), so `UINT64_MAX`
        /// never reaches the core.
        bool waitOn(DeviceImpl& impl, ID3D12Fence* fence, uint64_t value, uint64_t timeoutNs, std::string_view what, std::string_view entry)
        {
            core::DeviceCounts& counts = impl.core.counts();
            core::DeviceCounts::add(counts.nativeHostCalls);
            const uint64_t completed = fence->GetCompletedValue();
            if (completed >= value)
            {
                if (completed == UINT64_MAX)
                {
                    abortIfRemoved(impl, entry);
                }
                return true;
            }
            if (timeoutNs == 0)
            {
                return false;
            }
            core::DeviceCounts::add(counts.nativeHostCalls, 2);
            if (!d3d12::waitForFence(fence, value, timeoutNs, what))
            {
                return false;
            }
            // The wait does not say what value it saw: a removed device ends it at once, so the device is asked.
            abortIfRemoved(impl, entry);
            return true;
        }

        //==============================================================================================================
        /// Creates a fence at `initial` on `impl`'s device into `fence` and names it `label`; returns the error of a refusal.
        Result<void> createNativeFence(DeviceImpl& impl, uint64_t initial, std::string_view label, d3d12::Com<ID3D12Fence>& fence)
        {
            if (const HRESULT made = impl.device()->CreateFence(initial, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())); FAILED(made))
            {
                return std::unexpected(d3d12::failure(impl, made, std::format("ID3D12Device::CreateFence for \"{}\"", label)));
            }
            d3d12::setName(fence.get(), label);
            return {};
        }
    } // namespace

    namespace d3d12 {
        //==============================================================================================================
        Result<void> FrameState::create(DeviceImpl& impl)
        {
            return createNativeFence(impl, 0, "rojorhi.frame", m_frame);
        }

        //==============================================================================================================
        ID3D12Fence* FrameState::fence() const
        {
            return m_frame.get();
        }

        //==============================================================================================================
        void FrameState::keep(DeviceImpl& impl, ID3D12Fence* fence)
        {
            // The graphics queue signals no progress: the frame retirement that released the fence covers its submissions. A
            // compute or copy submission made before the fence's destruction is behind the queue's progress value now.
            Kept kept{ Com<ID3D12Fence>(fence), {} };
            for (const QueueType type : { QueueType::Compute, QueueType::Copy })
            {
                const size_t index = static_cast<size_t>(type);
                kept.progress[index] = impl.queues[index].progress.value.load(std::memory_order_relaxed);
            }
            std::lock_guard lock(m_mutex);
            if (m_kept.size() == m_kept.capacity())
            {
                core::DeviceCounts::add(impl.core.counts().cpuAllocations);
            }
            m_kept.push_back(std::move(kept));
        }

        //==============================================================================================================
        void FrameState::releasePassed(DeviceImpl& impl)
        {
            std::lock_guard lock(m_mutex);
            if (m_kept.empty())
            {
                return;
            }
            std::array<uint64_t, 3> needed = {};
            for (const Kept& kept : m_kept)
            {
                for (size_t i = 0; i < needed.size(); ++i)
                {
                    needed[i] = std::max(needed[i], kept.progress[i]);
                }
            }
            for (size_t i = 0; i < needed.size(); ++i)
            {
                // A queue whose creation failed has no progress fence and ran nothing; teardown releases what it would hold.
                ID3D12Fence* progress = impl.queues[i].progress.fence.get();
                if (needed[i] > m_seen[i] && progress != nullptr)
                {
                    m_seen[i] = std::max(m_seen[i], progress->GetCompletedValue());
                    core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
                }
            }
            std::erase_if(m_kept, [&](const Kept& kept) {
                for (size_t i = 0; i < kept.progress.size(); ++i)
                {
                    if (kept.progress[i] > m_seen[i])
                    {
                        return false;
                    }
                }
                return true;
            });
        }

        //==============================================================================================================
        void FrameState::releaseAll()
        {
            std::lock_guard lock(m_mutex);
            m_kept.clear();
        }

        //==============================================================================================================
        size_t FrameState::keptCount() const
        {
            std::lock_guard lock(m_mutex);
            return m_kept.size();
        }

        //==============================================================================================================
        Result<void> createQueues(DeviceImpl& impl)
        {
            for (size_t i = 0; i < impl.queues.size(); ++i)
            {
                QueueImpl& queue = impl.queues[i];
                D3D12_COMMAND_QUEUE_DESC desc = {};
                desc.Type = kQueueTypes[i];
                desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
                desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
                desc.NodeMask = 0;
                if (const HRESULT made = impl.device()->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.native.put())); FAILED(made))
                {
                    return std::unexpected(failure(impl, made, std::format("ID3D12Device::CreateCommandQueue for \"{}\"", kQueueLabels[i])));
                }
                setName(queue.native.get(), kQueueLabels[i]);
                const std::string progress = std::format("{}.progress", kQueueLabels[i]);
                if (Result<void> made = createNativeFence(impl, 0, progress, queue.progress.fence); !made)
                {
                    return made;
                }
            }
            return {};
        }

        //==============================================================================================================
        void idleQueues(DeviceImpl& impl)
        {
            for (size_t i = 0; i < impl.queues.size(); ++i)
            {
                QueueImpl& queue = impl.queues[i];
                // Creation that failed midway leaves the later queues, or a queue's progress fence, unmade.
                if (!queue.native || !queue.progress.fence)
                {
                    continue;
                }
                // The signal is teardown's own and is not counted: no counter is read after the device is destroyed.
                const uint64_t value = queue.progress.value.load(std::memory_order_relaxed) + 1;
                queue.progress.value.store(value, std::memory_order_relaxed);
                if (const HRESULT signalled = queue.native->Signal(queue.progress.fence.get(), value); FAILED(signalled))
                {
                    if (deviceRemoved(signalled))
                    {
                        // A removed device runs nothing more, so no queue holds work and every object can go.
                        reportRemoval(impl, "destroyDevice");
                        return;
                    }
                    ROJORHI_LOG_ERROR(
                        "ID3D12CommandQueue::Signal on \"{}\" at destroyDevice failed with HRESULT 0x{:08X}", kQueueLabels[i], hresultBits(signalled)
                    );
                    std::abort();
                }
                const std::string what = std::format("the progress fence of \"{}\" at destroyDevice", kQueueLabels[i]);
                waitForFence(queue.progress.fence.get(), value, UINT64_MAX, what);
            }
        }

        //==============================================================================================================
        void retireFence(void* context, core::FenceRecord& record)
        {
            auto* fence = static_cast<ID3D12Fence*>(record.native);
            if (fence == nullptr)
            {
                return;
            }
            DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
            // The fence's reference moves to the kept list; a queue may still wait on or signal it.
            record.native = nullptr;
            impl.frames.keep(impl, fence);
            impl.frames.releasePassed(impl);
        }

        //==============================================================================================================
        void waitFrame(void* context, uint64_t frame)
        {
            // The fence is named, and a slow wait's warning names the frame it waits for, so no label is formatted per frame.
            DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
            waitOn(impl, impl.frames.fence(), frame, UINT64_MAX, "the frame fence", "Device::beginFrame");
        }

        //==============================================================================================================
        void reuseSlot(void* context, uint32_t slot)
        {
            // The frame wait before it has proven the slot's work complete, the compute and copy work the frame joined included.
            DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
            impl.lists.reset(slot);
            impl.descriptors.resetTransient(slot);
            impl.frames.releasePassed(impl);
        }

        //==============================================================================================================
        void requireSucceeded(DeviceImpl& impl, HRESULT result, std::string_view what)
        {
            if (SUCCEEDED(result))
            {
                return;
            }
            if (deviceRemoved(result))
            {
                reportRemoval(impl, what);
            }
            ROJORHI_LOG_ERROR("{} failed with HRESULT 0x{:08X}", what, hresultBits(result));
            std::abort();
        }
    } // namespace d3d12

    //==================================================================================================================
    void Device::beginFrame()
    {
        // The core waits for the frame whose slot it reuses through the waitFrame hook, after its own checks.
        DeviceImpl::of(*this).core.beginFrame();
    }

    //==================================================================================================================
    void Device::endFrame()
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        impl.core.endFrame();
        // Queued after every graphics submission of the frame, which joined the frame's compute and copy work.
        ID3D12CommandQueue* graphics = impl.queues[static_cast<size_t>(QueueType::Graphics)].native.get();
        d3d12::requireSucceeded(impl, graphics->Signal(impl.frames.fence(), impl.core.frameNumber()), "ID3D12CommandQueue::Signal of the frame fence");
        core::DeviceCounts::add(impl.core.counts().nativeSubmitCalls);
    }

    //==================================================================================================================
    Result<Fence> Device::createFence(uint64_t initial, std::string_view label)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::FenceRecord record = core::prepareFence(initial, label);
        d3d12::Com<ID3D12Fence> fence;
        if (Result<void> made = createNativeFence(impl, initial, record.label, fence); !made)
        {
            return std::unexpected(std::move(made.error()));
        }
        record.native = fence.get();
        const Fence handle = impl.core.pool<Fence>().allocate(std::move(record));
        if (handle.bits == 0)
        {
            // The record was not taken, so the fence's one reference is still `fence`'s and goes with it.
            return std::unexpected(core::tableFull("fence", impl.core.pool<Fence>().capacity()));
        }
        // The record owns the reference now; the fence's retirement passes it on.
        (void)fence.detach();
        return handle;
    }

    //==================================================================================================================
    uint64_t Device::completedValue(Fence fence)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        // The handle rules fire before the payload is read.
        (void)impl.core.pool<Fence>().checked(fence, false);
        auto* native = static_cast<ID3D12Fence*>(impl.core.pool<Fence>().native(fence));
        const uint64_t completed = native->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            abortIfRemoved(impl, "Device::completedValue");
        }
        core::completeFence(impl.core, FenceValue{ fence, completed });
        // The core counts the native read and tells the validation tracker what the value orders complete.
        return core::completedValue(impl.core, fence);
    }

    //==================================================================================================================
    bool Device::wait(FenceValue value, uint64_t timeoutNs)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        // VU-rojo-queue-0008; a value the core already saw reached needs no native wait.
        if (core::checkWait(impl.core, value, timeoutNs))
        {
            return true;
        }
        auto* native = static_cast<ID3D12Fence*>(impl.core.pool<Fence>().native(value.fence));
        if (!waitOn(impl, native, value.value, timeoutNs, "a caller fence", "Device::wait"))
        {
            return false;
        }
        core::completeFence(impl.core, value);
        // Publishes the reached value to the validation tracker.
        return core::checkWait(impl.core, value, timeoutNs);
    }

    //==================================================================================================================
    void Device::destroy(Fence fence)
    {
        // The fence's retirement keeps its native fence until every queue has passed the submissions made before this call.
        core::destroyFence(DeviceImpl::of(*this).core, fence);
    }
} // namespace rojoRHI
