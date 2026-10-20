//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Device.cpp
/// @brief Defines the device entries of the D3D12 backend, which creates no device, so only createDevice is reachable.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI {
    namespace core {
        //==============================================================================================================
        DeviceCore& coreOf(Device)
        {
            d3d12::notImplemented("core::coreOf(Device)");
        }
    } // namespace core

    //==================================================================================================================
    Result<Device> createDevice(const DeviceDesc& desc)
    {
        if (Result<void> valid = core::validateDeviceDesc(desc); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        return std::unexpected(Error{ ErrorCategory::Unsupported, "createDevice is not supported by the D3D12 backend, which creates no device" });
    }

    //==================================================================================================================
    void destroyDevice(Device)
    {
        d3d12::notImplemented("destroyDevice");
    }

    //==================================================================================================================
    const Capabilities& Device::capabilities() const
    {
        d3d12::notImplemented("Device::capabilities");
    }

    //==================================================================================================================
    FormatCapabilities Device::formatCapabilities(Format) const
    {
        d3d12::notImplemented("Device::formatCapabilities");
    }

    //==================================================================================================================
    void Device::beginFrame()
    {
        d3d12::notImplemented("Device::beginFrame");
    }

    //==================================================================================================================
    void Device::endFrame()
    {
        d3d12::notImplemented("Device::endFrame");
    }

    //==================================================================================================================
    Result<Buffer> Device::createBuffer(const BufferDesc&, std::span<const std::byte>, Placement)
    {
        return d3d12::unsupported("Device::createBuffer");
    }

    //==================================================================================================================
    Result<Heap> Device::createHeap(const HeapDesc&)
    {
        return d3d12::unsupported("Device::createHeap");
    }

    //==================================================================================================================
    PlacementRequirements Device::placementRequirements(const BufferDesc&)
    {
        d3d12::notImplemented("Device::placementRequirements(BufferDesc)");
    }

    //==================================================================================================================
    PlacementRequirements Device::placementRequirements(const TextureDesc&)
    {
        d3d12::notImplemented("Device::placementRequirements(TextureDesc)");
    }

    //==================================================================================================================
    Result<Texture> Device::createTexture(const TextureDesc&, std::span<const TextureData>, Placement)
    {
        return d3d12::unsupported("Device::createTexture");
    }

    //==================================================================================================================
    Result<ViewIndex> Device::createView(Texture, const TextureViewDesc&)
    {
        return d3d12::unsupported("Device::createView");
    }

    //==================================================================================================================
    Result<SamplerIndex> Device::createSampler(const SamplerDesc&)
    {
        return d3d12::unsupported("Device::createSampler");
    }

    //==================================================================================================================
    Result<IndexRange> Device::reserveViews(uint32_t)
    {
        return d3d12::unsupported("Device::reserveViews");
    }

    //==================================================================================================================
    void Device::writeView(IndexRange, uint32_t, Texture, const TextureViewDesc&)
    {
        d3d12::notImplemented("Device::writeView");
    }

    //==================================================================================================================
    Result<IndexRange> Device::reserveSamplers(uint32_t)
    {
        return d3d12::unsupported("Device::reserveSamplers");
    }

    //==================================================================================================================
    void Device::writeSampler(IndexRange, uint32_t, const SamplerDesc&)
    {
        d3d12::notImplemented("Device::writeSampler");
    }

    //==================================================================================================================
    void Device::release(IndexRange)
    {
        d3d12::notImplemented("Device::release");
    }

    //==================================================================================================================
    Queue Device::queue(QueueType)
    {
        d3d12::notImplemented("Device::queue");
    }

    //==================================================================================================================
    Result<ShaderLibrary> Device::loadShaderLibrary(std::span<const std::byte>, std::string_view)
    {
        return d3d12::unsupported("Device::loadShaderLibrary");
    }

    //==================================================================================================================
    Result<Pipeline> Device::createPipeline(const GraphicsPipelineDesc&)
    {
        return d3d12::unsupported("Device::createPipeline(GraphicsPipelineDesc)");
    }

    //==================================================================================================================
    Result<Pipeline> Device::createPipeline(const ComputePipelineDesc&)
    {
        return d3d12::unsupported("Device::createPipeline(ComputePipelineDesc)");
    }

    //==================================================================================================================
    Result<std::vector<std::byte>> Device::pipelineCacheData()
    {
        return d3d12::unsupported("Device::pipelineCacheData");
    }

    //==================================================================================================================
    Result<Fence> Device::createFence(uint64_t, std::string_view)
    {
        return d3d12::unsupported("Device::createFence");
    }

    //==================================================================================================================
    Result<Upscaler> Device::createUpscaler(const UpscalerDesc&)
    {
        return d3d12::unsupported("Device::createUpscaler");
    }

    //==================================================================================================================
    uint64_t Device::completedValue(Fence)
    {
        d3d12::notImplemented("Device::completedValue");
    }

    //==================================================================================================================
    bool Device::wait(FenceValue, uint64_t)
    {
        d3d12::notImplemented("Device::wait");
    }

    //==================================================================================================================
    Addr Device::address(Buffer, uint64_t)
    {
        d3d12::notImplemented("Device::address");
    }

    //==================================================================================================================
    std::byte* Device::cpuPointer(Buffer)
    {
        d3d12::notImplemented("Device::cpuPointer");
    }

    //==================================================================================================================
    TileShape Device::tileShape(Texture)
    {
        d3d12::notImplemented("Device::tileShape(Texture)");
    }

    //==================================================================================================================
    TileShape Device::tileShape(Buffer)
    {
        d3d12::notImplemented("Device::tileShape(Buffer)");
    }

    //==================================================================================================================
    std::span<const ScopeTiming> Device::scopeTimings()
    {
        d3d12::notImplemented("Device::scopeTimings");
    }

    //==================================================================================================================
    Counters Device::counters() const
    {
        d3d12::notImplemented("Device::counters");
    }

    //==================================================================================================================
    Result<void> Device::beginCapture(std::string_view)
    {
        return d3d12::unsupported("Device::beginCapture");
    }

    //==================================================================================================================
    void Device::endCapture()
    {
        d3d12::notImplemented("Device::endCapture");
    }

    //==================================================================================================================
    void Device::destroy(Buffer)
    {
        d3d12::notImplemented("Device::destroy(Buffer)");
    }

    //==================================================================================================================
    void Device::destroy(Texture)
    {
        d3d12::notImplemented("Device::destroy(Texture)");
    }

    //==================================================================================================================
    void Device::destroy(Pipeline)
    {
        d3d12::notImplemented("Device::destroy(Pipeline)");
    }

    //==================================================================================================================
    void Device::destroy(ShaderLibrary)
    {
        d3d12::notImplemented("Device::destroy(ShaderLibrary)");
    }

    //==================================================================================================================
    void Device::destroy(Heap)
    {
        d3d12::notImplemented("Device::destroy(Heap)");
    }

    //==================================================================================================================
    void Device::destroy(Fence)
    {
        d3d12::notImplemented("Device::destroy(Fence)");
    }

    //==================================================================================================================
    void Device::destroy(Upscaler)
    {
        d3d12::notImplemented("Device::destroy(Upscaler)");
    }

    //==================================================================================================================
    void Device::destroy(ViewIndex)
    {
        d3d12::notImplemented("Device::destroy(ViewIndex)");
    }

    //==================================================================================================================
    void Device::destroy(SamplerIndex)
    {
        d3d12::notImplemented("Device::destroy(SamplerIndex)");
    }
} // namespace rojoRHI
