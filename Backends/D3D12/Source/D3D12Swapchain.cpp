//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Swapchain.cpp
/// @brief Stubs the swapchain entries of the D3D12 backend: it creates no swapchain until presentation is implemented.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include <rojoRHI/RHI.h>

#include "Core/Swapchains.h"

namespace rojoRHI {
    namespace core {
        //==============================================================================================================
        SwapchainCore& coreOf(Swapchain)
        {
            d3d12::notImplemented("core::coreOf(Swapchain)");
        }
    } // namespace core

    //==================================================================================================================
    Result<Swapchain> Device::createSwapchain(const SwapchainDesc&)
    {
        return d3d12::unsupported("Device::createSwapchain");
    }

    //==================================================================================================================
    void Device::destroy(Swapchain)
    {
        d3d12::notImplemented("Device::destroy(Swapchain)");
    }

    //==================================================================================================================
    void Swapchain::resize(Extent2D)
    {
        d3d12::notImplemented("Swapchain::resize");
    }

    //==================================================================================================================
    Texture Swapchain::acquire()
    {
        d3d12::notImplemented("Swapchain::acquire");
    }
} // namespace rojoRHI
