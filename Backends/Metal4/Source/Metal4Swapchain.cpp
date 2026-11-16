//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Swapchain.cpp
/// @brief Implements the public swapchain entries for the Metal 4 backend: a swapchain configures and presents to a `CAMetalLayer`.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Swapchain.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Swapchains.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Formats.h"
#include "Metal4Presentation.h"

#include <CoreGraphics/CoreGraphics.h>
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <objc/message.h>
#include <objc/runtime.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <utility>

namespace rojoRHI {
    namespace {
        /// How long `Swapchain::acquire` waits for the latency limit, and after which it asks the layer for no further
        /// drawable; chapter 11's one second. `nextDrawable` keeps Core Animation's own one-second timeout on top of it.
        constexpr std::chrono::seconds kAcquireDeadline{ 1 };

        //==============================================================================================================
        /// Returns the name of the Core Graphics color space chapter 11 maps `colorSpace` to, or null for a value that is no
        /// enumerator.
        CFStringRef colorSpaceName(ColorSpace colorSpace)
        {
            switch (colorSpace)
            {
            case ColorSpace::SRGB: return kCGColorSpaceSRGB;
            case ColorSpace::ExtendedLinear: return kCGColorSpaceExtendedLinearSRGB;
            case ColorSpace::HDR10: return kCGColorSpaceITUR_2100_PQ;
            }
            return nullptr;
        }

        //==============================================================================================================
        /// Sets the layer's `wantsExtendedDynamicRangeContent`, which metal-cpp does not wrap, through the Objective-C runtime.
        void setWantsExtendedDynamicRange(CA::MetalLayer* layer, bool wants)
        {
            static const SEL selector = sel_registerName("setWantsExtendedDynamicRangeContent:");
            using Setter = void (*)(const void*, SEL, BOOL);
            reinterpret_cast<Setter>(&objc_msgSend)(layer, selector, wants ? YES : NO);
        }

        //==============================================================================================================
        /// Configures `layer` for a swapchain of `desc` on `device`: device, pixel format, the color space `colorSpace` and EDR,
        /// drawable count, display sync and framebuffer-only drawables. The caller holds an autorelease pool and keeps its
        /// reference to `colorSpace`.
        void configureLayer(CA::MetalLayer* layer, MTL::Device* device, CGColorSpaceRef colorSpace, const SwapchainDesc& desc)
        {
            layer->setDevice(device);
            layer->setPixelFormat(metal4::pixelFormat(desc.format));
            layer->setColorspace(colorSpace);
            // A display without EDR tone-maps the extended spaces, so every space is accepted on every Mac.
            setWantsExtendedDynamicRange(layer, desc.colorSpace != ColorSpace::SRGB);
            layer->setMaximumDrawableCount(desc.imageCount);
            layer->setDisplaySyncEnabled(desc.mode == PresentMode::Fifo);
            layer->setFramebufferOnly(true);
            // Without the timeout, nextDrawable could block for as long as every drawable is held.
            layer->setAllowsNextDrawableTimeout(true);
        }

        //==============================================================================================================
        /// Returns `size` in whole pixels, rounded to the nearest.
        Extent2D wholePixels(CGSize size)
        {
            return { static_cast<uint32_t>(std::lround(size.width)), static_cast<uint32_t>(std::lround(size.height)) };
        }

        //==============================================================================================================
        /// Returns the device that created `swapchain`.
        DeviceImpl& deviceOf(const SwapchainImpl& swapchain)
        {
            return *static_cast<DeviceImpl*>(swapchain.core.device->retireHooks().context);
        }

        //==============================================================================================================
        /// Waits until fewer than `maxLatency` presents of `latency` are pending or `deadline` passes. Returns false when the
        /// deadline passed first. A count already below the limit is read without the lock.
        bool waitForLatency(metal4::PresentLatency& latency, uint32_t maxLatency, std::chrono::steady_clock::time_point deadline)
        {
            const auto below = [&] { return latency.count.load(std::memory_order_acquire) < maxLatency; };
            if (below())
            {
                return true;
            }
            std::unique_lock lock(latency.mutex);
            return latency.presented.wait_until(lock, deadline, below);
        }

        //==============================================================================================================
        /// Takes drawables from `layer` until one has the extent `extent`, and returns it retained; null when the layer gives
        /// none or `deadline` passes first. Each `nextDrawable` is one native host call of `device`. A drawable of another
        /// extent, queued across a resize, goes back to the layer unpresented.
        CA::MetalDrawable* nextDrawable(DeviceImpl& device, CA::MetalLayer* layer, Extent2D extent, std::chrono::steady_clock::time_point deadline)
        {
            for (;;)
            {
                // Each attempt drains its own pool, so a drawable left unretained is released before the next is asked for.
                const metal4::AutoreleasePool pool;
                CA::MetalDrawable* drawable = layer->nextDrawable();
                core::DeviceCounts::add(device.core.counts().nativeHostCalls);
                if (drawable == nullptr)
                {
                    return nullptr;
                }
                const MTL::Texture* texture = drawable->texture();
                if (texture->width() == extent.width && texture->height() == extent.height)
                {
                    return drawable->retain();
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return nullptr;
                }
            }
        }

        //==============================================================================================================
        /// Lowers the pending presents of `latency` after Core Animation presented or dropped one of its drawables, and wakes
        /// the acquires that wait for the limit. The count changes under the mutex, so a waiter cannot miss the notification.
        void lowerPending(metal4::PresentLatency& latency)
        {
            {
                std::lock_guard lock(latency.mutex);
                latency.count.fetch_sub(1, std::memory_order_release);
            }
            latency.presented.notify_all();
        }
    } // namespace

    namespace metal4 {
        //==============================================================================================================
        bool isMetalLayer(const void* surface)
        {
            if (surface == nullptr)
            {
                return false;
            }
            static const SEL selector = sel_registerName("isKindOfClass:");
            static const Class layerClass = objc_lookUpClass("CAMetalLayer");
            const AutoreleasePool pool;
            using KindOf = BOOL (*)(const void*, SEL, Class);
            return reinterpret_cast<KindOf>(&objc_msgSend)(surface, selector, layerClass) != NO;
        }

        //==============================================================================================================
        void presentSwapchain(QueueImpl& queue, SwapchainImpl& swapchain)
        {
            DeviceImpl& device = *queue.device;
            // The core checks the call and releases the image's texture, deferred, so committed work still holds it.
            core::presentImage(device.core, queue.type, swapchain.core);
            const AutoreleasePool pool;
            // With no submission since the acquire, the queue's wait for the drawable is issued here, before its signal.
            flushPresentation(queue);
            CA::MetalDrawable* drawable = std::exchange(swapchain.native.drawable, nullptr);
            queue.native->signalDrawable(drawable);
            core::DeviceCounts::add(device.core.counts().nativeSubmitCalls);
            // The handler shares the counter, so it may run after the swapchain is freed.
            std::shared_ptr<PresentLatency> latency = swapchain.native.latency;
            latency->count.fetch_add(1, std::memory_order_relaxed);
            drawable->addPresentedHandler(MTL::DrawablePresentedHandlerFunction([latency](MTL::Drawable*) { lowerPending(*latency); }));
            drawable->present();
            core::DeviceCounts::add(device.core.counts().nativeHostCalls);
            drawable->release();
        }
    } // namespace metal4

    //==================================================================================================================
    SwapchainImpl& SwapchainImpl::of(Swapchain swapchain)
    {
        core::checkObject(swapchain.m_impl, "Swapchain");
        return *swapchain.m_impl;
    }

    //==================================================================================================================
    Swapchain SwapchainImpl::wrap(SwapchainImpl* impl)
    {
        Swapchain swapchain;
        swapchain.m_impl = impl;
        return swapchain;
    }

    //==================================================================================================================
    Result<Swapchain> Device::createSwapchain(const SwapchainDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        // The core checks the surface before anything reads it as a layer; the extent comes from the layer afterwards.
        core::SwapchainCore state = core::prepareSwapchain(impl.core, desc, {});
        const metal4::AutoreleasePool pool;
        // A color space Core Graphics cannot make fails the creation before the core's state or the layer changes, and before
        // the swapchain is allocated. The core's check has already counted the creation in `nativeCreateCalls`, which stays:
        // the backend cannot fail a creation before that check, and only a value that is no enumerator is expected here.
        const CFStringRef name = colorSpaceName(desc.colorSpace);
        CGColorSpaceRef colorSpace = name != nullptr ? CGColorSpaceCreateWithName(name) : nullptr;
        if (colorSpace == nullptr)
        {
            return std::unexpected(
                Error{ ErrorCategory::Unsupported, std::format("Core Graphics made no color space for ColorSpace {}", static_cast<int>(desc.colorSpace)) }
            );
        }
        auto swapchain = std::make_unique<SwapchainImpl>();
        core::DeviceCounts::add(impl.core.counts().cpuAllocations);
        swapchain->core = std::move(state);
        // The surface's uniqueness (VU-rojo-present-0006) is checked before the layer of another swapchain could change.
        impl.core.addSwapchain(swapchain->core);
        CA::MetalLayer* layer = static_cast<CA::MetalLayer*>(desc.nativeSurface);
        configureLayer(layer, impl.device, colorSpace, desc);
        CGColorSpaceRelease(colorSpace);
        swapchain->core.extent = wholePixels(layer->drawableSize());
        swapchain->native.layer = layer->retain();
        swapchain->native.latency = std::make_shared<metal4::PresentLatency>();
        // The layer vends its set once it has a device; the graphics queue holds it while the swapchain lives.
        if (MTL::ResidencySet* set = layer->residencySet(); set != nullptr)
        {
            swapchain->native.residencySet = set->retain();
        }
        else
        {
            ROJORHI_LOG_WARN("the layer of swapchain \"{}\" vends no residency set; its drawables rely on Metal's own residency", swapchain->core.label);
        }
        SwapchainImpl* raw = swapchain.get();
        metal4::fileSwapchain(impl.presentation, std::move(swapchain));
        return SwapchainImpl::wrap(raw);
    }

    //==================================================================================================================
    void Swapchain::resize(Extent2D extent)
    {
        SwapchainImpl& impl = SwapchainImpl::of(*this);
        core::resizeSwapchain(impl.core, extent);
        DeviceImpl& device = deviceOf(impl);
        // Freeing the layer's queued drawables of the old size inside `setDrawableSize` leaves an autoreleased device behind,
        // which this pool collects on the caller's thread.
        const metal4::AutoreleasePool pool;
        // Every list that used the images was submitted before the last ended frame's event was signalled on the graphics
        // queue, so once the event reaches that frame no work references them. Before the first frame there is none.
        if (const uint64_t frame = device.core.frameNumber(); frame != 0)
        {
            metal4::waitUntilSignaled(device.frameEvent, frame, "the frame event of device", device.core.label());
            core::DeviceCounts::add(device.core.counts().nativeHostCalls);
        }
        impl.native.layer->setDrawableSize(CGSize{ static_cast<CGFloat>(extent.width), static_cast<CGFloat>(extent.height) });
    }

    //==================================================================================================================
    Texture Swapchain::acquire()
    {
        SwapchainImpl& impl = SwapchainImpl::of(*this);
        core::checkAcquireImage(impl.core);
        // A swapchain made on a layer with no drawable size has no image to hand out until it is resized; the layer would
        // vend drawables of another extent.
        if (impl.core.extent.width == 0 || impl.core.extent.height == 0)
        {
            return {};
        }
        // One deadline bounds the latency wait and the search for a drawable of the swapchain's extent; a null result
        // changes no state.
        const auto deadline = std::chrono::steady_clock::now() + kAcquireDeadline;
        if (!waitForLatency(*impl.native.latency, impl.core.maxLatency, deadline))
        {
            return {};
        }
        DeviceImpl& device = deviceOf(impl);
        CA::MetalDrawable* drawable = nextDrawable(device, impl.native.layer, impl.core.extent, deadline);
        if (drawable == nullptr)
        {
            return {};
        }
        const metal4::AutoreleasePool pool;
        // The image's record holds the drawable's texture until it retires through the frame, since committed work does not
        // retain it; the swapchain holds the drawable until present.
        const Texture image = core::acquireImage(impl.core, drawable->texture()->retain());
        impl.native.drawable = drawable;
        metal4::fileDrawable(device.presentation, drawable);
        return image;
    }

    //==================================================================================================================
    void Device::destroy(Swapchain swapchain)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        SwapchainImpl& target = SwapchainImpl::of(swapchain);
        core::destroySwapchain(impl.core, target.core);
        // The swapchain and its layer outlive the call until its frame slot is reused, as the native objects of a deferred
        // resource do; freeing it then queues the detach of the layer's set for the graphics queue.
        metal4::retireSwapchain(impl, target);
    }
} // namespace rojoRHI
