//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Swapchain.cpp
/// @brief Implements the public swapchain entries for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Metal4Common.h"

namespace rojoRHI {

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
    Result<Swapchain> Device::createSwapchain([[maybe_unused]] const SwapchainDesc& desc)
    {
        DeviceImpl::of(*this);
        return std::unexpected(Error{ ErrorCategory::Unsupported, "Device::createSwapchain is not supported by the Metal 4 backend" });
    }

    //==================================================================================================================
    void Swapchain::resize([[maybe_unused]] Extent2D extent)
    {
        SwapchainImpl::of(*this);
        metal4::notImplemented("Swapchain::resize");
    }

    //==================================================================================================================
    Texture Swapchain::acquire()
    {
        SwapchainImpl::of(*this);
        metal4::notImplemented("Swapchain::acquire");
    }

    //==================================================================================================================
    void Device::destroy(Swapchain swapchain)
    {
        DeviceImpl::of(*this);
        SwapchainImpl::of(swapchain);
        metal4::notImplemented("Device::destroy(Swapchain)");
    }
} // namespace rojoRHI
