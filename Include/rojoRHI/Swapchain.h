//----------------------------------------------------------------------------------------------------------------------
/// @file Swapchain.h
/// @brief Declares the swapchain object handle.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Geometry.h>
#include <rojoRHI/Handles.h>

namespace rojoRHI {
    class Device;

    /// A presentation surface and its images. It wraps one backend pointer: copies refer to the same swapchain, and a
    /// default-constructed one refers to none (VU-rojo-core-0004). It lives from `Device::createSwapchain` to
    /// `Device::destroy`; using it after that is undefined behaviour no tier detects. It is used only with the device that
    /// created it (VU-rojo-core-0005). One thread at a time uses a swapchain, ordered with the `Queue::present` that consumes
    /// it.
    class Swapchain
    {
      public:
        /// Sets the extent of every image to `extent` and replaces the images; the next `acquire` returns an image of the new
        /// extent. Presentation setup outside the frame loop: no frame is open (VU-rojo-present-0007), no image is acquired
        /// and not yet presented (VU-rojo-present-0008), and both dimensions are above zero (VU-rojo-present-0009).
        void resize(Extent2D extent);

        /// Returns the next image, labelled after the swapchain, with undefined contents, inside an open frame
        /// (VU-rojo-present-0010) with no image of this swapchain acquired and not yet presented (VU-rojo-present-0011). It
        /// returns a null texture when no image became free within one second. The image is used only as a color attachment
        /// and in texture barriers (VU-rojo-present-0012), is never destroyed by the caller (VU-rojo-present-0013), and its
        /// first barrier names `Layout::Undefined` as the old layout (VU-rojo-present-0014). Its handle is stale after
        /// `Queue::present`. Thread class swapchain.
        Texture acquire();

      private:
        friend class Device;
        friend struct SwapchainImpl;
        struct SwapchainImpl* m_impl = nullptr;
    };
} // namespace rojoRHI
