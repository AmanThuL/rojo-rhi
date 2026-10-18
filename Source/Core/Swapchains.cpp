//----------------------------------------------------------------------------------------------------------------------
/// @file Swapchains.cpp
/// @brief Implements the swapchain record and the checks of the presentation rules.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Swapchains.h"

#include "Base/Assert.h"
#include "Base/Log.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Label.h"
#include "Core/Records.h"
#include "Core/Tracker/Tracker.h"

#include <cstdlib>
#include <utility>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        /// Returns the one format chapter 11 lists for `colorSpace`.
        [[maybe_unused]] Format formatOf(ColorSpace colorSpace)
        {
            switch (colorSpace)
            {
            case ColorSpace::SRGB: return Format::BGRA8Unorm;
            case ColorSpace::ExtendedLinear: return Format::RGBA16Float;
            case ColorSpace::HDR10: return Format::RGB10A2Unorm;
            }
            return Format::Undefined;
        }

        //==============================================================================================================
        /// Ends the hold on the acquired image of `swapchain`: its texture is released, deferred, and the image is available.
        void releaseAcquired(DeviceCore& core, SwapchainCore& swapchain)
        {
            SwapchainImage& image = swapchain.images[swapchain.acquired];
            core.pool<Texture>().release(image.texture, core.releaseFrame());
            forgetTexture(core, image.texture);
            image = SwapchainImage{};
            swapchain.acquired = SwapchainCore::kNoImage;
        }
    } // namespace

    //==================================================================================================================
    SwapchainCore prepareSwapchain(DeviceCore& core, const SwapchainDesc& desc, Extent2D extent)
    {
        ROJORHI_VU(desc.nativeSurface != nullptr, "VU-rojo-present-0001", "a swapchain is created with a null nativeSurface");
        checkSurface(core, desc.nativeSurface);
        ROJORHI_VU(
            desc.format == formatOf(desc.colorSpace),
            "VU-rojo-present-0003",
            "the color space {} does not admit the format {}",
            static_cast<int>(desc.colorSpace),
            static_cast<int>(desc.format)
        );
        ROJORHI_VU(desc.imageCount == 2 || desc.imageCount == 3, "VU-rojo-present-0004", "imageCount is {}, not 2 or 3", desc.imageCount);
        ROJORHI_VU(
            desc.maxLatency >= 1 && desc.maxLatency <= desc.imageCount,
            "VU-rojo-present-0005",
            "maxLatency is {}, which is not in [1, imageCount = {}]",
            desc.maxLatency,
            desc.imageCount
        );
        SwapchainCore swapchain;
        swapchain.device = &core;
        swapchain.label = resolveLabel(desc.label, "rojorhi.swapchain.unnamed");
        swapchain.surface = desc.nativeSurface;
        swapchain.format = desc.format;
        swapchain.colorSpace = desc.colorSpace;
        swapchain.mode = desc.mode;
        swapchain.maxLatency = desc.maxLatency;
        swapchain.extent = extent;
        swapchain.images.resize(desc.imageCount);
        DeviceCounts::add(core.counts().nativeCreateCalls);
        return swapchain;
    }

    //==================================================================================================================
    void checkSurface([[maybe_unused]] const DeviceCore& core, [[maybe_unused]] const void* surface)
    {
        ROJORHI_VU(
            core.surfaceCheck() == nullptr || core.surfaceCheck()(surface),
            "VU-rojo-present-0002",
            "nativeSurface {} is not the surface type of the compiled backend",
            surface
        );
    }

    //==================================================================================================================
    void checkAcquireImage([[maybe_unused]] const SwapchainCore& swapchain)
    {
        ROJORHI_VU(swapchain.device->frameOpen(), "VU-rojo-present-0010", "acquire was called on the swapchain \"{}\" with no frame open", swapchain.label);
        ROJORHI_VU(
            swapchain.acquired == SwapchainCore::kNoImage,
            "VU-rojo-present-0011",
            "acquire was called on the swapchain \"{}\" while an image acquired in frame {} is not presented",
            swapchain.label,
            swapchain.acquiredFrame
        );
    }

    //==================================================================================================================
    Texture acquireImage(SwapchainCore& swapchain, void* native)
    {
        DeviceCore& core = *swapchain.device;
        checkAcquireImage(swapchain);
        const uint32_t index = swapchain.next;
        SwapchainImage& image = swapchain.images[index];
        ROJORHI_INTERNAL_ASSERT(image.state == ImageState::Available, "a swapchain with no image acquired holds an image that is not available");
        TextureRecord record;
        record.label = swapchain.label;
        record.type = TextureType::Tex2D;
        record.format = swapchain.format;
        record.extent = { swapchain.extent.width, swapchain.extent.height, 1 };
        record.usage = TextureUsage::RenderTarget;
        record.swapchainImage = true;
        // The pool publishes the payload before the handle exists, so a lock-free `native` read never sees it change.
        record.native = native;
        // The image is a native memory object for the counters but no native creation, so the pool does not count it itself.
        image.texture = core.pool<Texture>().allocate(std::move(record), false);
        if (image.texture.bits == 0)
        {
            // Acquire cannot fail by the reference, so an image that finds the texture table full ends the process in every build.
            ROJORHI_LOG_ERROR(
                "the swapchain \"{}\" cannot acquire an image: the texture table of {} objects is full", swapchain.label, core.pool<Texture>().capacity() - 1
            );
            std::abort();
        }
        // Every acquire hands out an image with undefined contents, whose first barrier starts from Undefined (chapter 11).
        trackTexture(core, image.texture, swapchain.label, 1, 1, Layout::Undefined, true, Heap{}, 0, 0, false);
        DeviceCounts::add(core.counts().gpuAllocations);
        image.state = ImageState::Acquired;
        swapchain.acquired = index;
        swapchain.acquiredFrame = core.frameNumber();
        swapchain.next = (index + 1) % static_cast<uint32_t>(swapchain.images.size());
        return image.texture;
    }

    //==================================================================================================================
    void presentImage(DeviceCore& core, [[maybe_unused]] QueueType type, SwapchainCore& swapchain)
    {
        checkSameDevice(core, *swapchain.device, "Swapchain");
        ROJORHI_VU(
            type == QueueType::Graphics, "VU-rojo-present-0015", "present was called on a queue of type {}, not the graphics queue", static_cast<int>(type)
        );
        ROJORHI_VU(
            swapchain.acquired != SwapchainCore::kNoImage,
            "VU-rojo-present-0016",
            "present was called on the swapchain \"{}\", which holds no acquired image",
            swapchain.label
        );
        ROJORHI_VU(
            core.frameOpen() && swapchain.acquiredFrame == core.frameNumber(),
            "VU-rojo-present-0017",
            "present was called on the swapchain \"{}\" outside frame {}, the frame of its acquire",
            swapchain.label,
            swapchain.acquiredFrame
        );
        // The image is judged before its release stops the tracker following it.
        checkPresentLayout(core, swapchain.images[swapchain.acquired].texture);
        // With VU-rojo-present-0016 compiled out, presenting with no acquired image is undefined in Release, as misuse is elsewhere in the core.
        // The image's texture retires through the frame, so a backend's native present may still read it after this returns.
        releaseAcquired(core, swapchain);
    }

    //==================================================================================================================
    void resizeSwapchain(SwapchainCore& swapchain, Extent2D extent)
    {
        ROJORHI_VU(!swapchain.device->frameOpen(), "VU-rojo-present-0007", "resize was called on the swapchain \"{}\" while a frame is open", swapchain.label);
        ROJORHI_VU(
            swapchain.acquired == SwapchainCore::kNoImage,
            "VU-rojo-present-0008",
            "resize was called on the swapchain \"{}\" while an image is acquired and not presented",
            swapchain.label
        );
        ROJORHI_VU(extent.width > 0 && extent.height > 0, "VU-rojo-present-0009", "resize was called with the extent {} by {}", extent.width, extent.height);
        swapchain.extent = extent;
    }

    //==================================================================================================================
    void destroySwapchain(DeviceCore& core, SwapchainCore& swapchain)
    {
        checkSameDevice(core, *swapchain.device, "Swapchain");
        if (swapchain.acquired != SwapchainCore::kNoImage)
        {
            releaseAcquired(core, swapchain);
        }
        core.removeSwapchain(swapchain);
    }
} // namespace rojoRHI::core
