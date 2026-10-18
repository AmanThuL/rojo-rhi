//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Presentation.cpp
/// @brief Implements the presentation state of a Metal 4 device: swapchain filing and retirement, and the graphics queue's flush.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Presentation.h"

#include "Base/Assert.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Metal4Common.h"
#include "Metal4Device.h"

#include <Metal/Metal.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace rojoRHI::metal4 {
    namespace {
        //==============================================================================================================
        /// Applies the queued changes of `presentation` to `queue` in order and releases each change's reference to its set. A
        /// native call is counted on `calls` when it is not null. The caller holds `presentation.mutex` and an autorelease pool.
        void applyChanges(Presentation& presentation, MTL4::CommandQueue* queue, std::atomic<uint64_t>* calls)
        {
            for (const ResidencySetChange& change : presentation.changes)
            {
                auto found = std::find_if(presentation.attachments.begin(), presentation.attachments.end(), [&](const ResidencySetAttachment& held) {
                    return held.set == change.set;
                });
                if (change.attach)
                {
                    if (found == presentation.attachments.end())
                    {
                        presentation.attachments.push_back(ResidencySetAttachment{ change.set, 0 });
                        found = presentation.attachments.end() - 1;
                    }
                    if (found->count++ == 0)
                    {
                        queue->addResidencySet(change.set);
                        if (calls != nullptr)
                        {
                            core::DeviceCounts::add(*calls);
                        }
                    }
                }
                else
                {
                    ROJORHI_INTERNAL_ASSERT(
                        found != presentation.attachments.end() && found->count > 0, "a residency set is detached more often than it was attached"
                    );
                    if (--found->count == 0)
                    {
                        queue->removeResidencySet(change.set);
                        if (calls != nullptr)
                        {
                            core::DeviceCounts::add(*calls);
                        }
                        presentation.attachments.erase(found);
                    }
                }
                change.set->release();
            }
            presentation.changes.clear();
            presentation.pending.store(false, std::memory_order_relaxed);
        }

        //==============================================================================================================
        /// Releases the native objects of `swapchain` and queues the detach of its layer's set, which takes over the
        /// swapchain's reference to it. The caller holds an autorelease pool and not `presentation.mutex`.
        void freeSwapchain(Presentation& presentation, SwapchainImpl& swapchain)
        {
            SwapchainNative& native = swapchain.native;
            if (native.drawable != nullptr)
            {
                // Released unpresented, the drawable goes back to the layer. It was unfiled when the swapchain was destroyed,
                // so a queue wait for it was issued in a frame no later than the destroying one, which has finished.
                native.drawable->release();
                native.drawable = nullptr;
            }
            if (native.residencySet != nullptr)
            {
                std::lock_guard lock(presentation.mutex);
                presentation.changes.push_back(ResidencySetChange{ native.residencySet, false });
                presentation.pending.store(true, std::memory_order_release);
                native.residencySet = nullptr;
            }
            if (native.layer != nullptr)
            {
                native.layer->release();
                native.layer = nullptr;
            }
        }
    } // namespace

    //==================================================================================================================
    void fileSwapchain(Presentation& presentation, std::unique_ptr<SwapchainImpl> swapchain)
    {
        std::lock_guard lock(presentation.mutex);
        if (MTL::ResidencySet* set = swapchain->native.residencySet; set != nullptr)
        {
            presentation.changes.push_back(ResidencySetChange{ set->retain(), true });
            presentation.pending.store(true, std::memory_order_release);
        }
        presentation.swapchains.push_back(std::move(swapchain));
    }

    //==================================================================================================================
    void retireSwapchain(DeviceImpl& impl, SwapchainImpl& swapchain)
    {
        Presentation& presentation = impl.presentation;
        const AutoreleasePool pool;
        // The frame is read under the lock so a slot reuse cannot slip between reading it and filing the entry.
        std::lock_guard lock(presentation.mutex);
        const auto found =
            std::find_if(presentation.swapchains.begin(), presentation.swapchains.end(), [&](const auto& live) { return live.get() == &swapchain; });
        ROJORHI_INTERNAL_ASSERT(found != presentation.swapchains.end(), "a swapchain that passed the device check is not in the device's list");
        // A drawable still filed has seen no graphics submission since its acquire. It will never be written or presented, so
        // no queue waits for it; the swapchain keeps its own reference until it is freed.
        if (MTL::Drawable* drawable = swapchain.native.drawable; drawable != nullptr)
        {
            const auto filed = std::find(presentation.drawables.begin(), presentation.drawables.end(), drawable);
            if (filed != presentation.drawables.end())
            {
                (*filed)->release();
                presentation.drawables.erase(filed);
            }
        }
        presentation.retiredSwapchains[impl.core.releaseFrame() % impl.core.framesInFlight()].push_back(std::move(*found));
        presentation.swapchains.erase(found);
    }

    //==================================================================================================================
    void fileDrawable(Presentation& presentation, MTL::Drawable* drawable)
    {
        std::lock_guard lock(presentation.mutex);
        presentation.drawables.push_back(drawable->retain());
        presentation.pending.store(true, std::memory_order_release);
    }

    //==================================================================================================================
    void flushPresentation(QueueImpl& queue)
    {
        if (queue.type != QueueType::Graphics)
        {
            return;
        }
        Presentation& presentation = queue.device->presentation;
        if (!presentation.pending.load(std::memory_order_acquire))
        {
            return;
        }
        std::atomic<uint64_t>& calls = queue.device->core.counts().nativeSubmitCalls;
        const AutoreleasePool pool;
        std::lock_guard lock(presentation.mutex);
        // The waits and the changes are applied in one locked section, so nothing filed meanwhile is left behind when
        // `pending` is cleared.
        for (MTL::Drawable* drawable : presentation.drawables)
        {
            queue.native->wait(drawable);
            core::DeviceCounts::add(calls);
            // The swapchain still holds the drawable. A filed drawable's swapchain is live, so it is freed no earlier than the
            // reuse of a frame slot that comes after this one, and that reuse waits for the frame.
            drawable->release();
        }
        presentation.drawables.clear();
        applyChanges(presentation, queue.native, &calls);
    }

    //==================================================================================================================
    void freeSlotSwapchains(DeviceImpl& impl, uint32_t slot)
    {
        Presentation& presentation = impl.presentation;
        std::vector<std::unique_ptr<SwapchainImpl>> freed;
        {
            std::lock_guard lock(presentation.mutex);
            freed.swap(presentation.retiredSwapchains[slot]);
        }
        if (freed.empty())
        {
            return;
        }
        // A swapchain is no pool record, so the core's retire loop cannot see it: this is the one place that does.
        core::DeviceCounts::add(impl.core.counts().deferredRetired, freed.size());
        core::DeviceCounts::add(impl.core.counts().nativeCreateCalls, freed.size());
        const AutoreleasePool pool;
        for (const std::unique_ptr<SwapchainImpl>& swapchain : freed)
        {
            freeSwapchain(presentation, *swapchain);
        }
        freed.clear();
    }

    //==================================================================================================================
    void releaseSwapchains(DeviceImpl& impl)
    {
        Presentation& presentation = impl.presentation;
        const AutoreleasePool pool;
        std::vector<std::unique_ptr<SwapchainImpl>> freed;
        {
            std::lock_guard lock(presentation.mutex);
            // The queues are drained, so no wait for a filed drawable is issued any more.
            for (MTL::Drawable* drawable : presentation.drawables)
            {
                drawable->release();
            }
            presentation.drawables.clear();
            freed = std::move(presentation.swapchains);
            presentation.swapchains.clear();
            for (std::vector<std::unique_ptr<SwapchainImpl>>& retired : presentation.retiredSwapchains)
            {
                std::move(retired.begin(), retired.end(), std::back_inserter(freed));
                retired.clear();
            }
        }
        for (const std::unique_ptr<SwapchainImpl>& swapchain : freed)
        {
            freeSwapchain(presentation, *swapchain);
        }
        freed.clear();
        // The queues are drained, so the graphics queue takes the queued changes now; device destruction counts nothing.
        std::lock_guard lock(presentation.mutex);
        applyChanges(presentation, impl.queues[static_cast<size_t>(QueueType::Graphics)].native, nullptr);
        ROJORHI_INTERNAL_ASSERT(presentation.attachments.empty(), "a residency set is still attached after every swapchain was freed");
    }

    //==================================================================================================================
    uint32_t attachmentCount(Presentation& presentation, const MTL::ResidencySet* set)
    {
        std::lock_guard lock(presentation.mutex);
        const auto found =
            std::find_if(presentation.attachments.begin(), presentation.attachments.end(), [&](const ResidencySetAttachment& held) { return held.set == set; });
        return found != presentation.attachments.end() ? found->count : 0;
    }
} // namespace rojoRHI::metal4
