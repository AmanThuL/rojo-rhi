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
#include "D3D12Timestamps.h"

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace rojoRHI {
    struct DeviceImpl;

    /// The D3D12 backend's queue: its type, the device that owns it, which is how a queue used with another device is told
    /// apart, the native queue it submits to, and the progress fence only it signals.
    struct QueueImpl
    {
        DeviceImpl* device = nullptr;          ///< The device that owns the queue.
        QueueType type = QueueType::Graphics;  ///< The queue's type.
        d3d12::Com<ID3D12CommandQueue> native; ///< The native queue; null until `d3d12::createQueues` makes it.
        d3d12::QueueProgress progress;         ///< The fence only this queue signals, and its last submitted value.
        /// The native lists of the submission being made, kept so the next submission reuses the storage.
        std::vector<ID3D12CommandList*> executing;

        /// Returns the state `queue` wraps; checks VU-rojo-core-0004.
        static QueueImpl& of(Queue queue);

        /// Returns the queue handle that wraps `impl`.
        static Queue wrap(QueueImpl* impl);
    };

    /// The D3D12 backend's command list: the core's recording state, the device that acquired it, and the native list.
    struct CommandListImpl
    {
        core::CommandListCore core; ///< The state machine the recording rules read, and the device that acquired the list.
        d3d12::ListState state;     ///< The native list, its allocator and what recording keeps between commands.

        /// Returns the state `list` wraps; checks VU-rojo-core-0004.
        static CommandListImpl& of(CommandList list);

        /// Returns the list handle that wraps `impl`.
        static CommandList wrap(CommandListImpl* impl);
    };

    /// The D3D12 backend's swapchain: the core's record of the images and their state.
    struct SwapchainImpl
    {
        core::SwapchainCore core; ///< The description, extent and image state the presentation rules read.

        /// Returns the state `swapchain` wraps; checks VU-rojo-core-0004.
        static SwapchainImpl& of(Swapchain swapchain);

        /// Returns the swapchain handle that wraps `impl`.
        static Swapchain wrap(SwapchainImpl* impl);
    };

    /// The D3D12 backend's device: the shared core, which also keeps the capabilities it reports, what `d3d12::loadDevice`
    /// opened, and the device-wide native state. Members are declared in creation order; `destroyDevice` idles the queues,
    /// drains the core and then destroys them in reverse order, so every object made on the device is released before it.
    struct DeviceImpl
    {
        /// Creates the state of a device from a description `core::validateDeviceDesc` accepted and the device `opened`.
        DeviceImpl(const DeviceDesc& desc, d3d12::Loaded opened) : core(desc), loaded(std::move(opened))
        {
            for (size_t i = 0; i < queues.size(); ++i)
            {
                queues[i].device = this;
                queues[i].type = static_cast<QueueType>(i);
            }
        }

        core::DeviceCore core;                ///< Frame bookkeeping, pools, labels, capabilities and the format table.
        d3d12::Loaded loaded;                 ///< The device factory, DXGI factory, adapter and device.
        d3d12::MessageRouter messages;        ///< Routes the debug layer's messages while the device lives.
        d3d12::DeviceFeatures features;       ///< The optional features the device reports.
        std::array<QueueImpl, 3> queues = {}; ///< One queue per `QueueType`, indexed by its value.
        d3d12::FrameState frames;             ///< The frame fence and the fences of destroyed caller fences.
        d3d12::DescriptorHeaps descriptors;   ///< The shader-visible view and sampler heaps.
        d3d12::HeapDirectory directory;       ///< The buffer of each raw SRV, by view-heap index.
        d3d12::RootSignatures roots;          ///< The root and command signatures every pipeline shares.
        d3d12::Timestamps timestamps;         ///< The device-wide timestamp state.
        d3d12::Kernels kernels;               ///< The pipelines of the backend's own kernels.
        d3d12::Stager stager;                 ///< Copies initial data on the internal upload queue.
        d3d12::PipelineCache pipelineCache;   ///< The pipeline library every pipeline creation looks up first.
        d3d12::ListPools lists;               ///< The command lists of each frame slot.
        std::string adapterName;              ///< The storage `Capabilities::adapterName` views.
        std::string floorName;                ///< The storage `Capabilities::floorName` views.

        /// Returns the native device.
        ID3D12Device12* device() const
        {
            return loaded.device.get();
        }

        /// Returns the state `device` wraps; checks VU-rojo-core-0004.
        static DeviceImpl& of(Device device);

        /// Returns the device handle that wraps `impl`.
        static Device wrap(DeviceImpl* impl);
    };
} // namespace rojoRHI
