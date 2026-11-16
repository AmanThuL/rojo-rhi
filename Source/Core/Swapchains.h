//----------------------------------------------------------------------------------------------------------------------
/// @file Swapchains.h
/// @brief Declares the swapchain record and the checks of the presentation rules of chapter 11, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rojoRHI::core {
    class DeviceCore;

    /// Whether one image of a swapchain is free or held by the caller. A backend whose presents complete during the call has no
    /// state in between.
    enum class ImageState : uint8_t
    {
        Available, ///< The image can be acquired.
        Acquired   ///< The caller holds the image until `Queue::present`.
    };

    /// One image of a swapchain: its state and, while it is acquired, the texture handle that names it.
    struct SwapchainImage
    {
        ImageState state = ImageState::Available; ///< Free or held.
        Texture texture = {};                     ///< The handle `acquire` returned, valid until the image is presented.
    };

    /// What the core keeps about one live swapchain: its description, its extent and the state of each image. A backend's
    /// `SwapchainImpl` embeds it. Images are texture records that exist while acquired, so each acquire returns a fresh handle
    /// and a handle kept past `Queue::present` fails the generation check.
    struct SwapchainCore
    {
        /// The index `acquired` holds when no image is acquired.
        static constexpr uint32_t kNoImage = UINT32_MAX;

        DeviceCore* device = nullptr;             ///< The device that created the swapchain.
        std::string label;                        ///< The label the swapchain was created with; its images carry it.
        void* surface = nullptr;                  ///< The consumer's surface; the core only compares it.
        Format format = Format::BGRA8Unorm;       ///< Format of every image.
        ColorSpace colorSpace = ColorSpace::SRGB; ///< How the display interprets image values.
        PresentMode mode = PresentMode::Fifo;     ///< When a presented image reaches the display.
        uint32_t maxLatency = 2;                  ///< Most presents queued and not yet displayed.
        Extent2D extent = {};                     ///< Extent of every image.
        std::vector<SwapchainImage> images;       ///< One entry per image, in hand-out order.
        uint32_t next = 0;                        ///< The image the next acquire hands out.
        uint32_t acquired = kNoImage;             ///< The index of the image the caller holds, or `kNoImage`.
        uint64_t acquiredFrame = 0;               ///< The frame that image was acquired in.
    };

    /// Returns the core of the swapchain `swapchain` wraps (VU-rojo-core-0004). Each backend defines it.
    SwapchainCore& coreOf(Swapchain swapchain);

    /// Checks a swapchain description (VU-rojo-present-0001, 0003 to 0005, VU-rojo-core-0006, VU-rojo-core-0007) and returns the
    /// record of a swapchain of `extent` with every image available and no surface-uniqueness claim yet. A backend calls it
    /// first in `createSwapchain`, then `DeviceCore::addSwapchain` once the record is at its final address.
    SwapchainCore prepareSwapchain(DeviceCore& core, const SwapchainDesc& desc, Extent2D extent);

    /// Checks that the surface type suits the backend (VU-rojo-present-0002) through the predicate the backend registered with
    /// `DeviceCore::setSurfaceCheck`; a backend that registered none accepts every surface.
    void checkSurface(const DeviceCore& core, const void* surface);

    /// Checks that `swapchain` can acquire an image (VU-rojo-present-0010 and 0011) and changes nothing.
    void checkAcquireImage(const SwapchainCore& swapchain);

    /// Acquires the next image of `swapchain`: runs `checkAcquireImage`, takes the next available image, stores a `RenderTarget`
    /// texture record of the swapchain's format and extent for it and returns the handle. `native` is the backend's payload for
    /// the image; it is published with the handle, before the handle exists, and never changes while the handle is live. A
    /// backend whose native acquire can fail calls `checkAcquireImage` first, then its native acquire, then this.
    Texture acquireImage(SwapchainCore& swapchain, void* native);

    /// Presents the acquired image of `swapchain` on a queue of `type` of device `core`: checks VU-rojo-core-0005,
    /// VU-rojo-present-0015, 0016 and 0017, then releases the image's texture, deferred, and makes the image available. It
    /// counts no native call; the backend counts the native calls its present makes.
    void presentImage(DeviceCore& core, QueueType type, SwapchainCore& swapchain);

    /// Sets the extent of `swapchain` to `extent`: checks VU-rojo-present-0007 to 0009. The images of the old extent exist
    /// only while acquired, and none is, so nothing is left to retire.
    void resizeSwapchain(SwapchainCore& swapchain, Extent2D extent);

    /// Destroys `swapchain` on device `core`: checks VU-rojo-core-0005, releases an image still acquired, deferred, and removes
    /// the swapchain from the device's live set.
    void destroySwapchain(DeviceCore& core, SwapchainCore& swapchain);
} // namespace rojoRHI::core
