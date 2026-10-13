//----------------------------------------------------------------------------------------------------------------------
/// @file SwapchainDesc.h
/// @brief Declares the description of a swapchain.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/ColorSpace.h>
#include <rojoRHI/Format.h>
#include <rojoRHI/PresentMode.h>

#include <cstdint>
#include <string_view>

namespace rojoRHI {
    /// Describes a swapchain for `Device::createSwapchain`: the consumer-owned surface its images are displayed on and how
    /// they are shown. `nativeSurface` is not null (VU-rojo-present-0001) and is the surface type of the compiled backend
    /// (VU-rojo-present-0002); `format` is the one `colorSpace` admits (VU-rojo-present-0003); `imageCount` is 2 or 3
    /// (VU-rojo-present-0004); `maxLatency` is at least 1 and at most `imageCount` (VU-rojo-present-0005).
    struct SwapchainDesc
    {
        /// The surface: a `CA::MetalLayer*` on Metal 4, an `HWND` on D3D12.
        void* nativeSurface = nullptr;
        /// Format of every image; forms a pair with `colorSpace`.
        Format format = Format::BGRA8Unorm;
        /// How the display interprets image values.
        ColorSpace colorSpace = ColorSpace::SRGB;
        /// Images the swapchain rotates through.
        uint32_t imageCount = 3;
        /// When a presented image reaches the display.
        PresentMode mode = PresentMode::Fifo;
        /// Most presents of this swapchain that may be queued and not yet displayed; `acquire` waits while it is reached.
        uint32_t maxLatency = 2;
        /// Debug label; empty selects `rojorhi.swapchain.unnamed`.
        std::string_view label;
    };
} // namespace rojoRHI
