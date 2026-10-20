//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Device.h
/// @brief Declares the Metal 4 backend's device, queue, command list and swapchain state.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/FrameArena.h"
#include "Core/Swapchains.h"
#include "Metal4AddressMap.h"
#include "Metal4DrawState.h"
#include "Metal4Encoders.h"
#include "Metal4Kernels.h"
#include "Metal4ListState.h"
#include "Metal4PipelineCache.h"
#include "Metal4PipelineState.h"
#include "Metal4Presentation.h"
#include "Metal4Residency.h"
#include "Metal4Stager.h"
#include "Metal4Swapchain.h"
#include "Metal4Timestamps.h"

#include "CommitTiming.h"

#include <Metal/Metal.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rojoRHI {
    struct DeviceImpl;

    /// The Metal 4 backend's queue: its type, the device that owns it, which is how a queue used with another device is told
    /// apart, the native queue it submits to, and a progress event only it signals: after each submission that names a fence,
    /// and before and after the native updates of each tile mapping call. A native queue does not retain the events it waits
    /// on or signals, so a destroyed fence's event is kept until every queue's progress passes the value it had at the
    /// destruction (`metal4::releasePassedFences`).
    struct QueueImpl
    {
        DeviceImpl* device = nullptr;         ///< The device that owns the queue.
        QueueType type = QueueType::Graphics; ///< The queue's type.
        MTL4::CommandQueue* native = nullptr; ///< The native queue, owned by the device.
        /// Signalled by this queue alone, each time with the next progress value: after each submission that names a fence, and
        /// before and after the native updates of each `mapTiles` call, each of those two signals followed by a wait on the
        /// queue for it, which orders the updates against the queue's other work.
        MTL::SharedEvent* progress = nullptr;
        /// The last value submitted for signal on `progress`. Only `submit` and `mapTiles`, which the caller serialises per
        /// queue, write it; a fence's destruction reads it, after the submissions that named the fence.
        std::atomic<uint64_t> progressValue = 0;
        /// The command buffers of the submission being committed, kept for its capacity. Only `submit` touches it.
        std::vector<MTL4::CommandBuffer*> commitBuffers;
        /// The commit timing callback `metal4::setCommitTiming` set, or null; with it set, every commit carries feedback. Only
        /// a queue-class call writes it, so no submission runs while it changes; `submit` reads it.
        metal4::CommitTimingCallback commitCallback = nullptr;
        void* commitUser = nullptr; ///< The user pointer `commitCallback` is called with.
        uint64_t commitSerial = 0;  ///< The commits of this queue since the callback was set; `submit` writes it and `setCommitTiming` resets it.

        /// Returns the state `queue` wraps; checks VU-rojo-core-0004.
        static QueueImpl& of(Queue queue);

        /// Returns the queue handle that wraps `impl`.
        static Queue wrap(QueueImpl* impl);
    };

    /// The Metal 4 backend's command list: the core's recording state and the device that acquired it, the native objects the
    /// list records into, and what it keeps of its open render pass.
    struct CommandListImpl
    {
        core::CommandListCore core; ///< The state machine the recording rules read, and the device that acquired the list.
        metal4::ListState state;    ///< The list's allocator, command buffer and argument table, and its debug-group labels.
        metal4::Encoders encoders;  ///< The encoder the list has open, if any.
        metal4::DrawState draw;     ///< The open render pass's attachments and the list's render pass descriptor.

        /// Returns the state `list` wraps; checks VU-rojo-core-0004.
        static CommandListImpl& of(CommandList list);

        /// Returns the list handle that wraps `impl`.
        static CommandList wrap(CommandListImpl* impl);
    };

    /// The Metal 4 backend's swapchain: the core's record of the images and their state.
    struct SwapchainImpl
    {
        core::SwapchainCore core; ///< The description, extent and image state the presentation rules read.
        /// The layer and its residency set, retained until the swapchain is freed, the drawable of the acquired image and the
        /// pending presents shared with the presented handlers.
        metal4::SwapchainNative native;

        /// Returns the state `swapchain` wraps; checks VU-rojo-core-0004.
        static SwapchainImpl& of(Swapchain swapchain);

        /// Returns the swapchain handle that wraps `impl`.
        static Swapchain wrap(SwapchainImpl* impl);
    };

    namespace metal4 {
        /// The event of a destroyed fence, with one reference kept until each queue's progress event reaches the value
        /// `progress` holds for it: the queue's progress value when the fence was destroyed.
        struct RetiringEvent
        {
            MTL::SharedEvent* event = nullptr;     ///< The retained event.
            std::array<uint64_t, 3> progress = {}; ///< Per `QueueType`, the progress value the queue must reach.
        };
    } // namespace metal4

    /// The command lists of one frame slot: those taken since the slot was reused come first and the rest wait to be reused.
    struct ListPool
    {
        std::mutex mutex;                                    ///< Guards the pool, because acquisition is free-threaded.
        std::vector<std::unique_ptr<CommandListImpl>> lists; ///< Every list the slot ever needed.
        size_t used = 0;                                     ///< How many of `lists` the open frame has taken.
    };

    /// The Metal 4 backend's device: the shared core, which also keeps the capabilities it reports, its queues, the command lists
    /// of each frame slot and the native objects every resource shares. Each native pointer is one retained reference, released
    /// by `destroyDevice` in reverse order of creation.
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
        MTL::Device* device = nullptr;        ///< The system default device.
        /// The serializer `compiler` records every pipeline into, and the archive of `DeviceDesc::pipelineCache`.
        metal4::PipelineCache pipelineCache;
        MTL4::Compiler* compiler = nullptr;      ///< The device's one compiler.
        metal4::Kernels kernels;                 ///< The backend's own kernels, made with `compiler`.
        metal4::DepthStencilCache depthStencils; ///< The depth-stencil states pipelines share, one per distinct state.
        metal4::Residency residency;             ///< The residency set every queue of the device keeps attached.
        /// Copies initial data into Private memory on the internal upload queue, which also keeps `residency` attached.
        metal4::Stager stager;
        bool residencyAttached = false; ///< True while every queue, the upload queue included, holds `residency`.
        metal4::AddressMap addresses;   ///< Buffers by GPU address for `core::decodeAddr`; Debug builds only.
        metal4::Timestamps timestamps;  ///< The counter heaps of each frame slot, which time the scopes.
        /// Signalled on the graphics queue with each frame's number when the frame ends; `beginFrame` waits on it.
        MTL::SharedEvent* frameEvent = nullptr;
        std::mutex retiringMutex;                          ///< Guards `retiringEvents` and `progressSeen`.
        std::vector<metal4::RetiringEvent> retiringEvents; ///< Events of destroyed fences a queue may still use.
        std::array<uint64_t, 3> progressSeen = {};         ///< Per `QueueType`, the greatest progress value read so far.
        MTL::Buffer* viewTable = nullptr;                  ///< The `MTL::ResourceID` of each view slot, `maxViews` entries, Shared.
        MTL::Buffer* samplerTable = nullptr;               ///< The `MTL::ResourceID` of each sampler slot, `maxSamplers` entries, Shared.
        std::string adapterName;                           ///< The storage `Capabilities::adapterName` views.
        std::string floorName;                             ///< The storage `Capabilities::floorName` views.
        metal4::Presentation presentation;                 ///< The swapchains and what they leave to the graphics queue.

        /// Returns the state `device` wraps; checks VU-rojo-core-0004.
        static DeviceImpl& of(Device device);

        /// Returns the device handle that wraps `impl`.
        static Device wrap(DeviceImpl* impl);
    };

    namespace metal4 {
        /// Returns the open pass's render encoder, opening it through `openRender` when none is open. The test is inline, so a
        /// command recorded into an open render encoder makes no call to reach it.
        inline MTL4::RenderCommandEncoder* ensureRender(CommandListImpl& list)
        {
            if (MTL4::RenderCommandEncoder* open = list.encoders.renderEncoder(); open != nullptr) [[likely]]
            {
                return open;
            }
            return openRender(list);
        }

        /// Creates one frame-arena page of `size` bytes: an Upload buffer of the device's pool, resident in the device set, whose
        /// `contents()` the page maps. `context` is the `DeviceCore`; its retire hooks' context is the `DeviceImpl`. The arena
        /// calls it while it claims a page, under its lock.
        Result<core::ArenaPage> createArenaPage(void* context, uint64_t size);
    } // namespace metal4
} // namespace rojoRHI
