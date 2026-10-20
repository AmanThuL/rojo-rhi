//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Fences.cpp
/// @brief Stubs the D3D12 queues, fences and frame pacing: creation makes no queue yet, and the frame and fence entries abort.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Fences.h"

#include <rojoRHI/RHI.h>

#include "Core/Records.h"
#include "D3D12Common.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rojoRHI {
    namespace d3d12 {
        //==============================================================================================================
        Result<void> FrameState::create(DeviceImpl&)
        {
            return {};
        }

        //==============================================================================================================
        ID3D12Fence* FrameState::fence() const
        {
            return m_frame.get();
        }

        //==============================================================================================================
        void FrameState::keep(DeviceImpl&, ID3D12Fence*)
        {
            notImplemented("FrameState::keep");
        }

        //==============================================================================================================
        void FrameState::releasePassed(DeviceImpl&)
        {
            notImplemented("FrameState::releasePassed");
        }

        //==============================================================================================================
        void FrameState::releaseAll()
        {
        }

        //==============================================================================================================
        size_t FrameState::keptCount() const
        {
            notImplemented("FrameState::keptCount");
        }

        //==============================================================================================================
        Result<void> createQueues(DeviceImpl&)
        {
            return {};
        }

        //==============================================================================================================
        void idleQueues(DeviceImpl&)
        {
        }

        //==============================================================================================================
        void retireFence(void*, core::FenceRecord&)
        {
            notImplemented("d3d12::retireFence");
        }

        //==============================================================================================================
        void waitFrame(void*, uint64_t)
        {
            notImplemented("d3d12::waitFrame");
        }

        //==============================================================================================================
        void reuseSlot(void*, uint32_t)
        {
            notImplemented("d3d12::reuseSlot");
        }
    } // namespace d3d12

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
    Result<Fence> Device::createFence(uint64_t, std::string_view)
    {
        return d3d12::unsupported("Device::createFence");
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
    void Device::destroy(Fence)
    {
        d3d12::notImplemented("Device::destroy(Fence)");
    }
} // namespace rojoRHI
