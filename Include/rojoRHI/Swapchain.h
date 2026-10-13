//----------------------------------------------------------------------------------------------------------------------
/// @file Swapchain.h
/// @brief Declares the swapchain object handle.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

namespace rojoRHI {
    class Device;

    /// A presentation surface and its images. It wraps one backend pointer: copies refer to the same swapchain, and a
    /// default-constructed one refers to none. It lives from `Device::createSwapchain` to `Device::destroy`; using it after
    /// that is undefined behaviour no tier detects. Its members arrive with the presentation chapter.
    class Swapchain
    {
      public:
      private:
        friend class Device;
        struct SwapchainImpl* m_impl = nullptr;
    };
} // namespace rojoRHI
