//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Descriptors.cpp
/// @brief Stubs the D3D12 descriptor heaps: creation makes no heap yet and reports the floor's capacities.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Descriptors.h"

#include "D3D12Common.h"

#include <cstdint>

namespace rojoRHI::d3d12 {
    //==================================================================================================================
    Result<void> DescriptorHeaps::create(DeviceImpl&)
    {
        return {};
    }

    //==================================================================================================================
    uint32_t DescriptorHeaps::viewCapacity() const
    {
        return 1'000'000;
    }

    //==================================================================================================================
    uint32_t DescriptorHeaps::samplerCapacity() const
    {
        return 2'048;
    }

    //==================================================================================================================
    ID3D12DescriptorHeap* DescriptorHeaps::views() const
    {
        notImplemented("DescriptorHeaps::views");
    }

    //==================================================================================================================
    ID3D12DescriptorHeap* DescriptorHeaps::samplers() const
    {
        notImplemented("DescriptorHeaps::samplers");
    }

    //==================================================================================================================
    D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeaps::viewCpu(uint32_t) const
    {
        notImplemented("DescriptorHeaps::viewCpu");
    }

    //==================================================================================================================
    D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeaps::viewGpu(uint32_t) const
    {
        notImplemented("DescriptorHeaps::viewGpu");
    }

    //==================================================================================================================
    D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeaps::samplerCpu(uint32_t) const
    {
        notImplemented("DescriptorHeaps::samplerCpu");
    }

    //==================================================================================================================
    TransientSlot DescriptorHeaps::claimTransient(uint32_t)
    {
        notImplemented("DescriptorHeaps::claimTransient");
    }

    //==================================================================================================================
    void DescriptorHeaps::resetTransient(uint32_t)
    {
        notImplemented("DescriptorHeaps::resetTransient");
    }

    //==================================================================================================================
    Result<uint32_t> createRawViews(DeviceImpl&, ID3D12Resource*, uint64_t, bool)
    {
        notImplemented("d3d12::createRawViews");
    }

    //==================================================================================================================
    uint64_t rawViewElements(uint64_t)
    {
        notImplemented("d3d12::rawViewElements");
    }

    //==================================================================================================================
    void releaseRawViews(DeviceImpl&, uint32_t, uint32_t)
    {
        notImplemented("d3d12::releaseRawViews");
    }
} // namespace rojoRHI::d3d12
