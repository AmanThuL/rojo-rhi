//----------------------------------------------------------------------------------------------------------------------
/// @file NullDevice.h
/// @brief Declares the Null backend's device state.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Swapchains.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace rojoRHI {
    // The floor 2^29 - 1, the bytes one D3D12 raw view reaches (2^27 four-byte elements), so a Null address's `hi` offset
    // always fits a D3D12 address.
    constexpr uint64_t kNullMaxAddrOffset = 536'870'911;

    // The Null backend reads no surface, so every swapchain starts at this extent until `Swapchain::resize`.
    constexpr Extent2D kNullSurfaceExtent = { 1280, 720 };

    struct DeviceImpl;

    // `context` is the device's `core::DeviceCore`. A page is a zeroed host buffer of the pool, so an address into it
    // resolves like any buffer's, with the `IndirectArgs` usage because frame data may serve as indirect arguments.
    Result<core::ArenaPage> createArenaPage(void* context, uint64_t size);

    // Zeroed and aligned to `core::kArenaPageAlignment`; null when out of memory.
    std::byte* allocateHostMemory(uint64_t size);

    void freeHostMemory(void* memory);

    // `device` is how a queue used with another device is told apart. A submission completes during the call.
    struct QueueImpl
    {
        DeviceImpl* device = nullptr;
        QueueType type = QueueType::Graphics;

        static QueueImpl& of(Queue queue);

        static Queue wrap(QueueImpl* impl);
    };

    // Commands are not stored: a list is only the core's recording state.
    struct CommandListImpl
    {
        core::CommandListCore core;

        static CommandListImpl& of(CommandList list);

        static CommandList wrap(CommandListImpl* impl);
    };

    // Ignores the surface it was given and displays nothing.
    struct SwapchainImpl
    {
        core::SwapchainCore core;

        static SwapchainImpl& of(Swapchain swapchain);

        static Swapchain wrap(SwapchainImpl* impl);
    };

    // One frame slot's lists: the first `used` were taken since the slot was reused, the rest wait for reuse. `mutex` guards
    // the pool, because acquisition is free-threaded.
    struct ListPool
    {
        std::mutex mutex;
        std::vector<std::unique_ptr<CommandListImpl>> lists;
        size_t used = 0;
    };

    struct DeviceImpl
    {
        explicit DeviceImpl(const DeviceDesc& desc) : core(desc)
        {
            for (size_t i = 0; i < queues.size(); ++i)
            {
                queues[i].device = this;
                queues[i].type = static_cast<QueueType>(i);
            }
        }

        core::DeviceCore core;
        std::array<QueueImpl, 3> queues = {};
        std::array<ListPool, 3> listPools; // One per frame slot; a device uses its first `framesInFlight`.
        // Guards `swapchains` and `retiredSwapchains`: swapchain creation and destruction are free-threaded.
        std::mutex swapchainMutex;
        std::vector<std::unique_ptr<SwapchainImpl>> swapchains;
        // Swapchains destroyed in a frame of each slot, freed when that slot is reused, as a deferred resource is.
        std::array<std::vector<std::unique_ptr<SwapchainImpl>>, 3> retiredSwapchains;
        // Pipelines the cache blob covers: those carried in by `DeviceDesc::pipelineCache` and those created since.
        std::atomic<uint64_t> cachedPipelines = 0;

        static DeviceImpl& of(Device device);

        static Device wrap(DeviceImpl* impl);
    };
} // namespace rojoRHI
