//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Fences.cpp
/// @brief Implements the fence entries of the Metal 4 backend: each fence is an `MTL::SharedEvent`.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Fences.h"

#include <rojoRHI/RHI.h>

#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Fences.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Metal4Common.h"
#include "Metal4Device.h"

#include <Metal/Metal.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Returns `timeoutNs` in whole milliseconds, rounded up.
        uint64_t roundUpToMilliseconds(uint64_t timeoutNs)
        {
            return timeoutNs / 1'000'000 + (timeoutNs % 1'000'000 != 0 ? 1 : 0);
        }

        //==============================================================================================================
        /// Waits up to `milliseconds` for `event` to reach `value`, in slices of at most `metal4::kWaitSliceMilliseconds`;
        /// returns true when it reached it.
        bool waitFor(MTL::SharedEvent* event, uint64_t value, uint64_t milliseconds)
        {
            while (milliseconds > 0)
            {
                const uint64_t slice = std::min(milliseconds, metal4::kWaitSliceMilliseconds);
                if (event->waitUntilSignaledValue(value, slice))
                {
                    return true;
                }
                milliseconds -= slice;
            }
            return event->signaledValue() >= value;
        }
    } // namespace

    namespace metal4 {
        //==============================================================================================================
        void releasePassedFences(DeviceImpl& impl)
        {
            std::lock_guard lock(impl.retiringMutex);
            if (impl.retiringEvents.empty())
            {
                return;
            }
            std::array<uint64_t, 3> needed = {};
            for (const RetiringEvent& retiring : impl.retiringEvents)
            {
                for (size_t i = 0; i < needed.size(); ++i)
                {
                    needed[i] = std::max(needed[i], retiring.progress[i]);
                }
            }
            for (size_t i = 0; i < needed.size(); ++i)
            {
                if (needed[i] > impl.progressSeen[i])
                {
                    impl.progressSeen[i] = impl.queues[i].progress->signaledValue();
                    core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
                }
            }
            const AutoreleasePool pool;
            std::erase_if(impl.retiringEvents, [&](const RetiringEvent& retiring) {
                for (size_t i = 0; i < retiring.progress.size(); ++i)
                {
                    if (retiring.progress[i] > impl.progressSeen[i])
                    {
                        return false;
                    }
                }
                retiring.event->release();
                return true;
            });
        }

        //==============================================================================================================
        void releaseRetiringFences(DeviceImpl& impl)
        {
            std::lock_guard lock(impl.retiringMutex);
            for (const RetiringEvent& retiring : impl.retiringEvents)
            {
                retiring.event->release();
            }
            impl.retiringEvents.clear();
        }

        //==============================================================================================================
        void retireFence([[maybe_unused]] void* context, core::FenceRecord& record)
        {
            auto* event = static_cast<MTL::SharedEvent*>(record.native);
            if (event == nullptr)
            {
                return;
            }
            const AutoreleasePool pool;
            event->release();
            record.native = nullptr;
        }
    } // namespace metal4

    //==================================================================================================================
    Result<Fence> Device::createFence(uint64_t initial, std::string_view label)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::FenceRecord record = core::prepareFence(initial, label);
        const metal4::AutoreleasePool pool;
        MTL::SharedEvent* event = impl.device->newSharedEvent();
        if (event == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("the event of fence \"{}\" was not created", record.label) });
        }
        event->setSignaledValue(initial);
        event->setLabel(metal4::makeString(record.label));
        record.native = event;
        const Fence fence = impl.core.pool<Fence>().allocate(std::move(record));
        if (fence.bits == 0)
        {
            event->release();
            return std::unexpected(core::tableFull("fence", impl.core.pool<Fence>().capacity()));
        }
        return fence;
    }

    //==================================================================================================================
    uint64_t Device::completedValue(Fence fence)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        // The handle rules fire before the payload is read.
        (void)impl.core.pool<Fence>().checked(fence, false);
        const auto* event = static_cast<const MTL::SharedEvent*>(impl.core.pool<Fence>().native(fence));
        core::completeFence(impl.core, FenceValue{ fence, event->signaledValue() });
        // The core counts the call and tells the validation tracker what the value orders complete.
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
        auto* event = static_cast<MTL::SharedEvent*>(impl.core.pool<Fence>().native(value.fence));
        core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
        bool reached = true;
        if (timeoutNs == 0)
        {
            reached = event->signaledValue() >= value.value;
        }
        else if (timeoutNs == UINT64_MAX)
        {
            // The event carries the fence's label, read only if a warning is logged.
            metal4::waitUntilSignaled(event, value.value, "fence", {});
        }
        else
        {
            reached = waitFor(event, value.value, roundUpToMilliseconds(timeoutNs));
        }
        if (!reached)
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
        DeviceImpl& impl = DeviceImpl::of(*this);
        // The handle rules fire before the payload is read.
        (void)impl.core.pool<Fence>().checked(fence, false);
        auto* event = static_cast<MTL::SharedEvent*>(impl.core.pool<Fence>().native(fence));
        // Every submission that named the fence returned before this call. The fence's retirement covers those a frame joins,
        // and each queue's progress value covers the rest: the compute and copy queues' fence-only submissions and those with
        // lists and more than one signal.
        metal4::RetiringEvent retiring{ event, {} };
        for (size_t i = 0; i < impl.queues.size(); ++i)
        {
            retiring.progress[i] = impl.queues[i].progressValue.load(std::memory_order_relaxed);
        }
        if (event == nullptr)
        {
            core::destroyFence(impl.core, fence);
            return;
        }
        // The event is retained before the core may retire the fence: a frame that retires it releases the fence's own
        // reference, never this one.
        event->retain();
        core::destroyFence(impl.core, fence);
        {
            std::lock_guard lock(impl.retiringMutex);
            bool pending = false;
            for (size_t i = 0; i < impl.queues.size(); ++i)
            {
                pending = pending || retiring.progress[i] > impl.progressSeen[i];
            }
            if (!pending)
            {
                // Every queue was already seen past its progress; the fence's retirement releases the fence's reference.
                const metal4::AutoreleasePool pool;
                event->release();
                return;
            }
            if (impl.retiringEvents.size() == impl.retiringEvents.capacity())
            {
                core::DeviceCounts::add(impl.core.counts().cpuAllocations);
            }
            impl.retiringEvents.push_back(retiring);
        }
        // A program that never begins a frame still releases what the queues have passed, so the kept events stay bounded.
        metal4::releasePassedFences(impl);
    }
} // namespace rojoRHI
