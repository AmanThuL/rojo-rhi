//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Swapchain.cpp
/// @brief Stubs the swapchain entries of the D3D12 backend after their handle checks: it creates no swapchain until presentation is implemented.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include <rojoRHI/RHI.h>

#include "Core/Swapchains.h"
#include "D3D12Device.h"

namespace rojoRHI {
    namespace core {
        //==============================================================================================================
        SwapchainCore& coreOf(Swapchain swapchain)
        {
            return SwapchainImpl::of(swapchain).core;
        }
    } // namespace core

    //==================================================================================================================
    Result<Swapchain> Device::createSwapchain(const SwapchainDesc&)
    {
        return d3d12::unsupported("Device::createSwapchain");
    }

    //==================================================================================================================
    void Device::destroy(Swapchain swapchain)
    {
        // The handle checks run first, as on every backend, so a default-constructed handle reports VU-rojo-core-0004.
        DeviceImpl::of(*this);
        SwapchainImpl::of(swapchain);
        d3d12::notImplemented("Device::destroy(Swapchain)");
    }

    //==================================================================================================================
    void Swapchain::resize(Extent2D)
    {
        SwapchainImpl::of(*this);
        d3d12::notImplemented("Swapchain::resize");
    }

    //==================================================================================================================
    Texture Swapchain::acquire()
    {
        SwapchainImpl::of(*this);
        d3d12::notImplemented("Swapchain::acquire");
    }
} // namespace rojoRHI
