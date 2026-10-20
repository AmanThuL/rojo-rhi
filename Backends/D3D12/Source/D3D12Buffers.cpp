//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Buffers.cpp
/// @brief Stubs the D3D12 buffer and heap entries: the backend creates no buffer or heap yet.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Buffers.h"

#include <rojoRHI/RHI.h>

#include "Core/FrameArena.h"
#include "Core/Records.h"
#include "D3D12Common.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace rojoRHI {
    namespace d3d12 {
        //==============================================================================================================
        void retireBuffer(void*, core::BufferRecord&)
        {
            notImplemented("d3d12::retireBuffer");
        }

        //==============================================================================================================
        void retireHeap(void*, core::HeapRecord&)
        {
            notImplemented("d3d12::retireHeap");
        }

        //==============================================================================================================
        Result<core::ArenaPage> createArenaPage(void*, uint64_t)
        {
            return unsupported("d3d12::createArenaPage");
        }
    } // namespace d3d12

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
    void Device::destroy(Buffer)
    {
        d3d12::notImplemented("Device::destroy(Buffer)");
    }

    //==================================================================================================================
    void Device::destroy(Heap)
    {
        d3d12::notImplemented("Device::destroy(Heap)");
    }
} // namespace rojoRHI
