//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Swapchain.h
/// @brief Declares the native objects of a Metal 4 swapchain, its present, and the surface check of the backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace rojoRHI {
    struct QueueImpl;
    struct SwapchainImpl;
} // namespace rojoRHI

namespace rojoRHI::metal4 {
    /// The presents of one swapchain that are queued and not yet displayed. `Queue::present` raises the count before it presents
    /// a drawable; the drawable's presented handler, which Core Animation runs on a thread of its own, lowers it and notifies
    /// `presented`. The swapchain and each handler share it, so a handler that runs after the swapchain is freed is safe.
    struct PresentLatency
    {
        std::atomic<uint32_t> count = 0;   ///< The presents not yet displayed.
        std::mutex mutex;                  ///< Pairs with `presented`; a leaf, never held across a native call.
        std::condition_variable presented; ///< Notified each time a presented handler lowers `count`.
    };

    /// The native objects of one swapchain, each held as one retained reference until the swapchain is freed, at the
    /// retirement of the frame that destroyed it or at device destruction.
    struct SwapchainNative
    {
        CA::MetalLayer* layer = nullptr; ///< The consumer's layer, configured at creation.
        /// The residency set of the layer's drawables, attached to the graphics queue while the swapchain lives; null when the
        /// layer vends none.
        MTL::ResidencySet* residencySet = nullptr;
        /// The drawable of the acquired image, retained from `Swapchain::acquire` until `Queue::present` hands it to the display;
        /// null while no image is acquired. A swapchain freed while it holds one releases it unpresented.
        CA::MetalDrawable* drawable = nullptr;
        std::shared_ptr<PresentLatency> latency; ///< The swapchain's pending presents, shared with their presented handlers.
    };

    /// Returns true when `surface` is a `CAMetalLayer` or one of its subclasses, through `isKindOfClass:`. A null `surface` is
    /// no layer; a pointer that is not an Objective-C object is undefined, as any invalid pointer is.
    bool isMetalLayer(const void* surface);

    /// Presents the drawable `swapchain` holds on the graphics queue `queue`, after the core's checks: issues the queue's
    /// pending drawable waits, `signalDrawable`, then `present` with a presented handler that lowers the swapchain's pending
    /// presents, and releases the drawable. `Queue::present` calls it.
    void presentSwapchain(QueueImpl& queue, SwapchainImpl& swapchain);
} // namespace rojoRHI::metal4
