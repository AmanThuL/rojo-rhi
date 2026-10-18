//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Presentation.h
/// @brief Declares the presentation state of a Metal 4 device: its swapchains and the queue calls they leave to the graphics queue.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <Metal/Metal.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace rojoRHI {
    struct DeviceImpl;
    struct QueueImpl;
    struct SwapchainImpl;
} // namespace rojoRHI

namespace rojoRHI::metal4 {
    /// A change to the residency sets the graphics queue holds, queued by a thread that is not the queue's: the set, one
    /// retained reference released once the change is applied, and whether it joins or leaves the queue.
    struct ResidencySetChange
    {
        MTL::ResidencySet* set = nullptr; ///< The retained set.
        bool attach = false;              ///< True to attach the set, false to detach it.
    };

    /// How many swapchains hold one layer's residency set attached to the graphics queue. A new swapchain on a layer whose old
    /// swapchain has not retired attaches the set again, so the set stays on the queue until the last of them detaches it.
    struct ResidencySetAttachment
    {
        const MTL::ResidencySet* set = nullptr; ///< The layer's set.
        uint32_t count = 0;                     ///< The applied attaches less the applied detaches.
    };

    /// The presentation state of a device. Swapchain creation, destruction and `Swapchain::acquire` are no queue calls, so what
    /// they need of the graphics queue is queued here and applied by `flushPresentation`, on the queue's own calls. `mutex` is a
    /// leaf: nothing is taken while it is held, and it is not held across a call that blocks.
    struct Presentation
    {
        std::mutex mutex; ///< Guards every member but `pending`.
        /// True while `changes` or `drawables` holds work for the graphics queue. Set and cleared under `mutex`; read first
        /// without it, so a submission with nothing queued takes no lock.
        std::atomic<bool> pending = false;
        std::vector<ResidencySetChange> changes; ///< The queued changes, in the order they are applied.
        /// The drawables acquired since the graphics queue's last submission or present, each with one retained reference,
        /// released once the queue's wait for it is issued or its swapchain is destroyed.
        std::vector<MTL::Drawable*> drawables;
        std::vector<ResidencySetAttachment> attachments;        ///< The sets attached to the graphics queue, with their counts.
        std::vector<std::unique_ptr<SwapchainImpl>> swapchains; ///< The swapchains created and not yet destroyed.
        /// The swapchains destroyed in a frame of each slot, freed when that slot is reused, as a deferred resource is.
        std::array<std::vector<std::unique_ptr<SwapchainImpl>>, 3> retiredSwapchains;
    };

    /// Files `swapchain`, whose native objects are retained, among the live swapchains of `presentation`, and queues the
    /// attach of its layer's residency set, when it has one.
    void fileSwapchain(Presentation& presentation, std::unique_ptr<SwapchainImpl> swapchain);

    /// Moves `swapchain`, which the core has destroyed, from the live swapchains of `impl` to those of the frame slot its
    /// destruction retires with, and unfiles its drawable, so no queue waits for a drawable that is never presented.
    void retireSwapchain(DeviceImpl& impl, SwapchainImpl& swapchain);

    /// Files `drawable`, which a swapchain has just acquired, for the graphics queue's next submission or present to wait for,
    /// with a reference of its own.
    void fileDrawable(Presentation& presentation, MTL::Drawable* drawable);

    /// Applies the queued work of the device's presentation state to `queue` when it is the graphics queue: makes the queue
    /// wait for each filed drawable, then attaches a set whose count goes from zero to one and detaches one whose count goes to
    /// zero, each one counted native queue call. Any other queue, or a graphics queue with nothing queued, returns after at
    /// most one atomic load and takes no lock.
    void flushPresentation(QueueImpl& queue);

    /// Frees the swapchains destroyed in the frames of a reused frame slot: releases their native objects, an unpresented
    /// drawable among them, queues the detach of their sets and counts each as a deferred destruction and a native release.
    void freeSlotSwapchains(DeviceImpl& impl, uint32_t slot);

    /// Frees every swapchain of `impl`, live or retired, releases the filed drawables and applies every queued change to the
    /// graphics queue, uncounted, at device destruction, after the queues are drained.
    void releaseSwapchains(DeviceImpl& impl);

    /// Returns how many applied attaches hold `set` on the graphics queue; zero when it is not attached. A test seam.
    uint32_t attachmentCount(Presentation& presentation, const MTL::ResidencySet* set);
} // namespace rojoRHI::metal4
