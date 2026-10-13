//----------------------------------------------------------------------------------------------------------------------
/// @file NullDevice.cpp
/// @brief Implements the public device entries for the Null backend by calling the shared core.
//----------------------------------------------------------------------------------------------------------------------

#include "NullDevice.h"

#include <rojoRHI/Device.h>

#include "Core/DeviceCore.h"

#include <array>
#include <memory>

namespace rojoRHI {
    namespace {
        constexpr std::array<UpscalerKind, 4> kUpscalers = { UpscalerKind::MetalFX, UpscalerKind::DLSS, UpscalerKind::FSR, UpscalerKind::XeSS };

        //==============================================================================================================
        /// Each field takes the Floor column of the limits table in the reference where that column gives a number, and
        /// otherwise the smaller of the Metal 4 and D3D12 values, so a program that fits the Null device fits both planned
        /// backends. The comment names the source of each value.
        Limits nullLimits()
        {
            Limits limits;
            limits.maxViews = 1'000'000;      // Floor.
            limits.maxSamplers = 996;         // Floor.
            limits.maxRootBlockSize = 65'536; // Floor.
            limits.rootAlignment = 256;       // Floor.
            limits.frameDataAlignment = 256;  // Floor.
            limits.maxBufferSize =
                1ull << 32; // Unverified: the Floor column names no number ("device"); 4 GiB, an arbitrary value above the 2^32 - 1 Addr reach.
            limits.maxAddrOffset = 4'294'967'295;      // Floor, 2^32 - 1.
            limits.maxTextureDimension1D = 16'384;     // Floor.
            limits.maxTextureDimension2D = 16'384;     // Floor.
            limits.maxTextureDimension3D = 2'048;      // Floor.
            limits.maxTextureDimensionCube = 16'384;   // Floor.
            limits.maxTextureLayers = 2'048;           // Floor.
            limits.maxColorAttachments = 8;            // Floor.
            limits.maxColorBytesPerSample = 64;        // Floor.
            limits.maxSamplerAnisotropy = 16;          // Floor.
            limits.maxDrawIndirectCount = 65'535;      // Floor.
            limits.maxDispatchGroups = 65'535;         // Floor.
            limits.maxThreadsPerGroup = 1'024;         // Floor.
            limits.maxGroupSize[0] = 1'024;            // Floor.
            limits.maxGroupSize[1] = 1'024;            // Floor.
            limits.maxGroupSize[2] = 64;               // Floor.
            limits.maxGroupSharedMemory = 32'768;      // Floor.
            limits.maxMeshTaskGroups = 65'535;         // Floor.
            limits.maxMeshTaskGroupsTotal = 4'194'304; // Floor.
            return limits;
        }
    } // namespace

    //==================================================================================================================
    DeviceImpl& DeviceImpl::of(Device device)
    {
        core::checkObject(device.m_impl, "Device");
        return *device.m_impl;
    }

    //==================================================================================================================
    Device DeviceImpl::wrap(DeviceImpl* impl)
    {
        Device device;
        device.m_impl = impl;
        return device;
    }

    namespace core {
        //==============================================================================================================
        DeviceCore& coreOf(Device device)
        {
            return DeviceImpl::of(device).core;
        }
    } // namespace core

    //==================================================================================================================
    Result<Device> createDevice(const DeviceDesc& desc)
    {
        if (Result<void> valid = core::validateDeviceDesc(desc); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        auto impl = std::make_unique<DeviceImpl>(desc);
        Capabilities& capabilities = impl->core.capabilities();
        capabilities.adapterName = "Null";
        capabilities.floorName = "Null";
        capabilities.limits = nullLimits();
        capabilities.maxViews = capabilities.limits.maxViews;
        capabilities.maxSamplers = capabilities.limits.maxSamplers;
        capabilities.uploadInDeviceMemory = true;
        capabilities.placementSparse = true;
        capabilities.meshTasksIndirect = true;
        capabilities.conservativeRaster = true;
        capabilities.upscalers = kUpscalers;
        impl->core.setFormatTable(core::formatCeilings());
        return DeviceImpl::wrap(impl.release());
    }

    //==================================================================================================================
    void destroyDevice(Device device)
    {
        DeviceImpl& impl = DeviceImpl::of(device);
        impl.core.beginDestroy();
        delete &impl;
    }

    //==================================================================================================================
    const Capabilities& Device::capabilities() const
    {
        return DeviceImpl::of(*this).core.capabilities();
    }

    //==================================================================================================================
    FormatCapabilities Device::formatCapabilities(Format format) const
    {
        return DeviceImpl::of(*this).core.formatCapabilities(format);
    }

    //==================================================================================================================
    void Device::beginFrame()
    {
        DeviceImpl::of(*this).core.beginFrame();
    }

    //==================================================================================================================
    void Device::endFrame()
    {
        DeviceImpl::of(*this).core.endFrame();
    }

    //==================================================================================================================
    void Device::destroy(Buffer buffer)
    {
        DeviceImpl::of(*this).core.destroy(buffer);
    }

    //==================================================================================================================
    void Device::destroy(Texture texture)
    {
        DeviceImpl::of(*this).core.destroy(texture);
    }

    //==================================================================================================================
    void Device::destroy(Pipeline pipeline)
    {
        DeviceImpl::of(*this).core.destroy(pipeline);
    }

    //==================================================================================================================
    void Device::destroy(ShaderLibrary library)
    {
        DeviceImpl::of(*this).core.destroy(library);
    }

    //==================================================================================================================
    void Device::destroy(Heap heap)
    {
        DeviceImpl::of(*this).core.destroy(heap);
    }

    //==================================================================================================================
    void Device::destroy(Fence fence)
    {
        DeviceImpl::of(*this).core.destroy(fence);
    }

    //==================================================================================================================
    void Device::destroy(Upscaler upscaler)
    {
        DeviceImpl::of(*this).core.destroy(upscaler);
    }

    // The view and sampler heaps and swapchains do not exist yet, so releasing one has nothing to defer.

    //==================================================================================================================
    void Device::destroy(ViewIndex)
    {
        DeviceImpl::of(*this);
    }

    //==================================================================================================================
    void Device::destroy(SamplerIndex)
    {
        DeviceImpl::of(*this);
    }

    //==================================================================================================================
    void Device::destroy(Swapchain swapchain)
    {
        DeviceImpl::of(*this);
        core::checkObject(swapchain.m_impl, "Swapchain");
    }
} // namespace rojoRHI
