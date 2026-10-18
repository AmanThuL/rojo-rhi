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
    /// The Null device's `Limits::maxAddrOffset`: the 32-bit byte offset an address carries in its `hi` word, 2^32 - 1.
    constexpr uint64_t kNullMaxAddrOffset = 4'294'967'295;

    /// The extent a Null swapchain's images have until `Swapchain::resize`: the Null backend reads no surface, so it gives every
    /// swapchain the same one, a common window size.
    constexpr Extent2D kNullSurfaceExtent = { 1280, 720 };

    struct DeviceImpl;

    /// Creates one frame-arena page as a zeroed host buffer of the pool, so an address into it resolves like any buffer's; the
    /// context is the device's `core::DeviceCore`. The page has the `IndirectArgs` usage because the reference lets frame data
    /// serve as indirect arguments.
    Result<core::ArenaPage> createArenaPage(void* context, uint64_t size);

    /// The Null backend's queue: it knows its type and the device that owns it, which is how a queue used with another device
    /// is told apart. It executes nothing; a submission completes during the call.
    struct QueueImpl
    {
        DeviceImpl* device = nullptr;         ///< The device that owns the queue.
        QueueType type = QueueType::Graphics; ///< The queue's type.

        /// Returns the state `queue` wraps; checks VU-rojo-core-0004.
        static QueueImpl& of(Queue queue);

        /// Returns the queue handle that wraps `impl`.
        static Queue wrap(QueueImpl* impl);
    };

    /// The Null backend's command list: the core's recording state and the device that owns it. Commands are not stored.
    struct CommandListImpl
    {
        core::CommandListCore core; ///< The state machine the recording rules read, and the device that acquired the list.

        /// Returns the state `list` wraps; checks VU-rojo-core-0004.
        static CommandListImpl& of(CommandList list);

        /// Returns the list handle that wraps `impl`.
        static CommandList wrap(CommandListImpl* impl);
    };

    /// The Null backend's swapchain: the core's record of the images and their state. It ignores the surface it was given and
    /// displays nothing.
    struct SwapchainImpl
    {
        core::SwapchainCore core; ///< The description, extent and image state the presentation rules read.

        /// Returns the state `swapchain` wraps; checks VU-rojo-core-0004.
        static SwapchainImpl& of(Swapchain swapchain);

        /// Returns the swapchain handle that wraps `impl`.
        static Swapchain wrap(SwapchainImpl* impl);
    };

    /// The command lists of one frame slot: those taken since the slot was reused come first and the rest wait to be reused.
    struct ListPool
    {
        std::mutex mutex;                                    ///< Guards the pool, because acquisition is free-threaded.
        std::vector<std::unique_ptr<CommandListImpl>> lists; ///< Every list the slot ever needed.
        size_t used = 0;                                     ///< How many of `lists` the open frame has taken.
    };

    /// The Null backend's device: the shared core, which also keeps the capabilities it reports. It renders nothing.
    struct DeviceImpl
    {
        /// Creates the state of a device from a description `core::validateDeviceDesc` accepted.
        explicit DeviceImpl(const DeviceDesc& desc) : core(desc)
        {
            for (size_t i = 0; i < queues.size(); ++i)
            {
                queues[i].device = this;
                queues[i].type = static_cast<QueueType>(i);
            }
        }

        core::DeviceCore core;                ///< Frame bookkeeping, pools, labels, capabilities and the format table.
        std::array<QueueImpl, 3> queues = {}; ///< One queue per `QueueType`, indexed by its value.
        std::array<ListPool, 3> listPools;    ///< One list pool per frame slot; a device uses its first `framesInFlight`.
        /// Guards `swapchains` and `retiredSwapchains`, because swapchain creation and destruction are free-threaded.
        std::mutex swapchainMutex;
        /// The swapchains created and not yet destroyed.
        std::vector<std::unique_ptr<SwapchainImpl>> swapchains;
        /// The swapchains destroyed in a frame of each slot, freed when that slot is reused, as a deferred resource is.
        std::array<std::vector<std::unique_ptr<SwapchainImpl>>, 3> retiredSwapchains;
        /// Pipelines the cache blob covers: those carried in by `DeviceDesc::pipelineCache` and those created since.
        std::atomic<uint64_t> cachedPipelines = 0;

        /// Returns the state `device` wraps; checks VU-rojo-core-0004.
        static DeviceImpl& of(Device device);

        /// Returns the device handle that wraps `impl`.
        static Device wrap(DeviceImpl* impl);
    };
} // namespace rojoRHI
