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
    // Queued by a thread that is not the graphics queue's; the set's retained reference is released once the change is
    // applied.
    struct ResidencySetChange
    {
        MTL::ResidencySet* set = nullptr;
        bool attach = false;
    };

    // A new swapchain on a layer whose old swapchain has not retired attaches the set again, so the set stays on the
    // queue until the last of them detaches it.
    struct ResidencySetAttachment
    {
        const MTL::ResidencySet* set = nullptr;
        uint32_t count = 0; // Applied attaches less applied detaches.
    };

    // Swapchain creation, destruction and `Swapchain::acquire` are no queue calls, so what they need of the graphics
    // queue is queued here and applied by `flushPresentation`. `mutex` is a leaf, never held across a blocking call.
    struct Presentation
    {
        std::mutex mutex; // Guards every member but `pending`.
        // Set and cleared under `mutex`; read first without it, so a submission with nothing queued takes no lock.
        std::atomic<bool> pending = false;
        std::vector<ResidencySetChange> changes; // In the order they are applied.
        // Each with one retained reference, released once the queue's wait for it is issued or its swapchain is
        // destroyed.
        std::vector<MTL::Drawable*> drawables;
        std::vector<ResidencySetAttachment> attachments;
        std::vector<std::unique_ptr<SwapchainImpl>> swapchains;
        // Freed when that slot is reused, as a deferred resource is.
        std::array<std::vector<std::unique_ptr<SwapchainImpl>>, 3> retiredSwapchains;
    };

    void fileSwapchain(Presentation& presentation, std::unique_ptr<SwapchainImpl> swapchain);

    // Unfiles its drawable, so no queue waits for a drawable that is never presented.
    void retireSwapchain(DeviceImpl& impl, SwapchainImpl& swapchain);

    // With a reference of its own.
    void fileDrawable(Presentation& presentation, MTL::Drawable* drawable);

    // Waits for each filed drawable first, then applies the attaches and detaches. Any other queue, or a graphics queue
    // with nothing queued, returns after at most one atomic load and takes no lock.
    void flushPresentation(QueueImpl& queue);

    void freeSlotSwapchains(DeviceImpl& impl, uint32_t slot);

    // At device destruction, after the queues are drained; uncounted.
    void releaseSwapchains(DeviceImpl& impl);

    // A test seam.
    uint32_t attachmentCount(Presentation& presentation, const MTL::ResidencySet* set);
} // namespace rojoRHI::metal4
