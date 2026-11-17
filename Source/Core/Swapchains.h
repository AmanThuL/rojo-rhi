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

    enum class ImageState : uint8_t
    {
        Available,
        Acquired
    };

    struct SwapchainImage
    {
        ImageState state = ImageState::Available;
        Texture texture = {}; // Valid until the image is presented.
    };

    // Images are texture records that exist only while acquired, so each acquire returns a fresh handle and a handle kept
    // past `Queue::present` fails the generation check. A backend's `SwapchainImpl` embeds it.
    struct SwapchainCore
    {
        static constexpr uint32_t kNoImage = UINT32_MAX;

        DeviceCore* device = nullptr;
        std::string label;
        void* surface = nullptr; // The consumer's; the core only compares it.
        Format format = Format::BGRA8Unorm;
        ColorSpace colorSpace = ColorSpace::SRGB;
        PresentMode mode = PresentMode::Fifo;
        uint32_t maxLatency = 2;
        Extent2D extent = {};
        std::vector<SwapchainImage> images;
        uint32_t next = 0;
        uint32_t acquired = kNoImage;
        uint64_t acquiredFrame = 0;
    };

    // Each backend defines it.
    SwapchainCore& coreOf(Swapchain swapchain);

    // The backend calls `DeviceCore::addSwapchain` once the record is at its final address.
    SwapchainCore prepareSwapchain(DeviceCore& core, const SwapchainDesc& desc, Extent2D extent);

    // Asks the predicate registered with `DeviceCore::setSurfaceCheck`; with none, every surface passes.
    void checkSurface(const DeviceCore& core, const void* surface);

    // Changes nothing.
    void checkAcquireImage(const SwapchainCore& swapchain);

    // `native` is published with the handle, before the handle exists, and never changes while it is live. A backend whose
    // native acquire can fail calls `checkAcquireImage`, then its native acquire, then this.
    Texture acquireImage(SwapchainCore& swapchain, void* native);

    // Counts no native call; the backend counts its present's.
    void presentImage(DeviceCore& core, QueueType type, SwapchainCore& swapchain);

    // Old-extent images exist only while acquired, and none is, so nothing is left to retire.
    void resizeSwapchain(SwapchainCore& swapchain, Extent2D extent);

    void destroySwapchain(DeviceCore& core, SwapchainCore& swapchain);
} // namespace rojoRHI::core
