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
    // Core Animation runs each presented handler on a thread of its own. The swapchain and its handlers share this
    // state, so a handler that runs after the swapchain is freed is safe.
    struct PresentLatency
    {
        std::atomic<uint32_t> count = 0;
        std::mutex mutex; // A leaf, never held across a native call.
        std::condition_variable presented;
    };

    // Each held as one retained reference until the swapchain is freed, at the retirement of the frame that destroyed
    // it or at device destruction.
    struct SwapchainNative
    {
        CA::MetalLayer* layer = nullptr;
        // Attached to the graphics queue while the swapchain lives; null when the layer vends none.
        MTL::ResidencySet* residencySet = nullptr;
        // Retained from `Swapchain::acquire` until `Queue::present`. A swapchain freed while it holds one releases it
        // unpresented.
        CA::MetalDrawable* drawable = nullptr;
        std::shared_ptr<PresentLatency> latency; // Shared with the presented handlers.
    };

    // Through `isKindOfClass:`; a pointer that is not an Objective-C object is undefined.
    bool isMetalLayer(const void* surface);

    // Issues the queue's pending drawable waits, then `signalDrawable`, then `present`.
    void presentSwapchain(QueueImpl& queue, SwapchainImpl& swapchain);
} // namespace rojoRHI::metal4
