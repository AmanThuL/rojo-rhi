//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Device.h
/// @brief Declares the D3D12 backend's device, queue, command list and swapchain state.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Swapchains.h"
#include "D3D12Addr.h"
#include "D3D12Capabilities.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Descriptors.h"
#include "D3D12Fences.h"
#include "D3D12Kernels.h"
#include "D3D12ListState.h"
#include "D3D12Loader.h"
#include "D3D12PipelineCache.h"
#include "D3D12RootSignature.h"
#include "D3D12Stager.h"
#include "D3D12SubmitTiming.h"
#include "D3D12Swapchain.h"
#include "D3D12Timestamps.h"
#include "D3D12Upscaler.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rojoRHI {
    struct DeviceImpl;

    struct QueueImpl
    {
        DeviceImpl* device = nullptr;
        QueueType type = QueueType::Graphics;
        d3d12::Com<ID3D12CommandQueue> native;
        d3d12::QueueProgress progress; // Signalled by this queue alone.
        // Kept so the next submission reuses the storage.
        std::vector<ID3D12CommandList*> executing;
        // Null while detached; its hooks read it with one relaxed load.
        std::atomic<d3d12::SubmitTimingState*> timing = nullptr;
        // Kept until the device is destroyed: a list acquired while one was attached may still write into it.
        std::vector<std::unique_ptr<d3d12::SubmitTimingState>> timings;

        // Checks VU-rojo-core-0004.
        static QueueImpl& of(Queue queue);

        static Queue wrap(QueueImpl* impl);
    };

    struct CommandListImpl
    {
        core::CommandListCore core;
        d3d12::ListState state;

        // Checks VU-rojo-core-0004.
        static CommandListImpl& of(CommandList list);

        static CommandList wrap(CommandListImpl* impl);
    };

    struct SwapchainImpl
    {
        core::SwapchainCore core;
        d3d12::SwapchainNative native;

        // Checks VU-rojo-core-0004.
        static SwapchainImpl& of(Swapchain swapchain);

        static Swapchain wrap(SwapchainImpl* impl);
    };

    // Members are declared in creation order; `destroyDevice` idles the queues, drains the core and then destroys them
    // in reverse, so every object made on the device is released before it.
    struct DeviceImpl
    {
        DeviceImpl(const DeviceDesc& desc, d3d12::Loaded opened) : core(desc), loaded(std::move(opened))
        {
            for (size_t i = 0; i < queues.size(); ++i)
            {
                queues[i].device = this;
                queues[i].type = static_cast<QueueType>(i);
            }
        }

        core::DeviceCore core;
        d3d12::Loaded loaded;
        d3d12::FsrRuntime fsr;
        d3d12::MessageRouter messages;
        d3d12::DeviceFeatures features;
        std::array<QueueImpl, 3> queues = {}; // Indexed by `QueueType`.
        d3d12::FrameState frames;
        d3d12::Presentation presentation;
        d3d12::DescriptorHeaps descriptors;
        d3d12::HeapDirectory directory;
        d3d12::RootSignatures roots;
        d3d12::Timestamps timestamps;
        d3d12::Kernels kernels;
        d3d12::Stager stager;
        d3d12::PipelineCache pipelineCache;
        d3d12::ListPools lists;
        std::string adapterName; // The storage `Capabilities::adapterName` views.
        std::string floorName;   // The storage `Capabilities::floorName` views.

        ID3D12Device12* device() const
        {
            return loaded.device.get();
        }

        // Checks VU-rojo-core-0004.
        static DeviceImpl& of(Device device);

        static Device wrap(DeviceImpl* impl);
    };
} // namespace rojoRHI
