//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Views.cpp
/// @brief Stubs the D3D12 view and sampler entries: the backend creates no view or sampler yet.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Views.h"

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <cstdint>

namespace rojoRHI {
    namespace d3d12 {
        //==============================================================================================================
        void retireView(void*, void*)
        {
            notImplemented("d3d12::retireView");
        }
    } // namespace d3d12

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
