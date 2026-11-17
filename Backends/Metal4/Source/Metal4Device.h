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

    // A native queue does not retain the events it waits on or signals. A fence's event lives until its frame slot
    // retires, which covers every graphics submission and every submission with lists and at most one signal; a
    // destroyed fence's event is kept beyond that until every queue's progress passes its value at the destruction
    // (`metal4::releasePassedFences`).
    struct QueueImpl
    {
        DeviceImpl* device = nullptr; // Tells apart a queue used with another device.
        QueueType type = QueueType::Graphics;
        MTL4::CommandQueue* native = nullptr; // Owned by the device.
        // Signalled by this queue alone with the next progress value: on the compute and copy queues after each
        // submission that names a fence and has no lists or more than one signal, and around the native updates of each
        // `mapTiles` call, each followed by a wait on the queue for it.
        MTL::SharedEvent* progress = nullptr;
        // Graphics queue only: a list's last encoder updates it after the work its owed barrier names, for that
        // barrier's dispatch stage, and each later encoder that dispatches waits on it, except a counted draw's
        // kernels, which a barrier to the dispatch stage would hold back. Only lists acquired while `carryFenced` is
        // set use it.
        MTL::Fence* carry = nullptr;
        // Graphics queue only: set, at most once, when a list opens the kernels-only encoder of a counted draw's
        // kernels; `Device::beginFrame` reads it.
        std::atomic<bool> kernelsRan = false;
        // Graphics queue only: set by the first `Device::beginFrame` after `kernelsRan`, never cleared. `beginFrame` is
        // of thread class frame, and every list of an earlier frame was submitted before that frame ended
        // (VU-rojo-device-0005), so every list that waits on no fence precedes, on the queue, every list that updates
        // `carry`.
        std::atomic<bool> carryFenced = false;
        // Only `submit` and `mapTiles`, which the caller serialises per queue, write it; a fence's destruction reads
        // it, after the submissions that named the fence.
        std::atomic<uint64_t> progressValue = 0;
        // Kept for its capacity. Only `submit` touches it.
        std::vector<MTL4::CommandBuffer*> commitBuffers;
        // Only a queue-class call writes it, so no submission runs while it changes.
        metal4::CommitTimingCallback commitCallback = nullptr;
        void* commitUser = nullptr;
        uint64_t commitSerial = 0; // `submit` writes it and `setCommitTiming` resets it.

        // Checks VU-rojo-core-0004.
        static QueueImpl& of(Queue queue);

        static Queue wrap(QueueImpl* impl);
    };

    struct CommandListImpl
    {
        core::CommandListCore core;
        metal4::ListState state;
        metal4::Encoders encoders;
        metal4::DrawState draw;

        // Checks VU-rojo-core-0004.
        static CommandListImpl& of(CommandList list);

        static CommandList wrap(CommandListImpl* impl);
    };

    struct SwapchainImpl
    {
        core::SwapchainCore core;
        metal4::SwapchainNative native;

        // Checks VU-rojo-core-0004.
        static SwapchainImpl& of(Swapchain swapchain);

        static Swapchain wrap(SwapchainImpl* impl);
    };

    namespace metal4 {
        // One reference is kept until each queue's progress event reaches `progress`, the queue's progress value when
        // the fence was destroyed.
        struct RetiringEvent
        {
            MTL::SharedEvent* event = nullptr;
            std::array<uint64_t, 3> progress = {};
        };
    } // namespace metal4

    // Those taken since the slot was reused come first; the rest wait to be reused.
    struct ListPool
    {
        std::mutex mutex; // Acquisition is free-threaded.
        std::vector<std::unique_ptr<CommandListImpl>> lists;
        size_t used = 0;
    };

    // Each native pointer is one retained reference, released by `destroyDevice` in reverse order of creation.
    struct DeviceImpl
    {
        // From a description `core::validateDeviceDesc` accepted.
        explicit DeviceImpl(const DeviceDesc& desc) : core(desc)
        {
            for (size_t i = 0; i < queues.size(); ++i)
            {
                queues[i].device = this;
                queues[i].type = static_cast<QueueType>(i);
            }
        }

        core::DeviceCore core;
        std::array<QueueImpl, 3> queues = {}; // Indexed by `QueueType`.
        std::array<ListPool, 3> listPools;    // A device uses its first `framesInFlight`.
        MTL::Device* device = nullptr;
        // False only when `device` is positively a bare driver class (`commandsMayAutorelease`); otherwise each native
        // draw, dispatch, copy, fill and encoder end opens a pool.
        bool commandPools = false;
        // Set by `DeviceDesc::debugMarkers` or `DeviceDesc::enableCapture` (ADR 0034). Elsewhere a group costs two
        // native calls and a label a converted copy and an allocation, so neither is sent; the command buffer's label
        // still names each list.
        bool markers = false;
        metal4::PipelineCache pipelineCache;
        MTL4::Compiler* compiler = nullptr;
        metal4::Kernels kernels; // Made with `compiler`.
        metal4::DepthStencilCache depthStencils;
        metal4::Residency residency; // Every queue keeps it attached.
        // Its upload queue also keeps `residency` attached.
        metal4::Stager stager;
        bool residencyAttached = false; // While every queue, the upload queue included, holds `residency`.
        metal4::AddressMap addresses;   // Debug builds only.
        metal4::Timestamps timestamps;
        // `beginFrame` waits on it.
        MTL::SharedEvent* frameEvent = nullptr;
        std::mutex retiringMutex;                          // Guards `retiringEvents` and `progressSeen`.
        std::vector<metal4::RetiringEvent> retiringEvents; // A queue may still use them.
        std::array<uint64_t, 3> progressSeen = {};         // Per `QueueType`, the greatest progress value read so far.
        MTL::Buffer* viewTable = nullptr;                  // `maxViews` entries, Shared.
        MTL::Buffer* samplerTable = nullptr;               // `maxSamplers` entries, Shared.
        MTL::Buffer* zeroRecord = nullptr;                 // Zeroes, Shared: the record a direct draw binds for drawId 0.
        std::string adapterName;                           // The storage `Capabilities::adapterName` views.
        std::string floorName;                             // The storage `Capabilities::floorName` views.
        std::array<UpscalerSupport, 1> upscalerSupport{};  // The storage `Capabilities::upscalers` views.
        metal4::Presentation presentation;

        // Checks VU-rojo-core-0004.
        static DeviceImpl& of(Device device);

        static Device wrap(DeviceImpl* impl);
    };

    namespace metal4 {
        // The test is inline, so a command recorded into an open render encoder makes no call to reach it.
        inline MTL4::RenderCommandEncoder* ensureRender(CommandListImpl& list)
        {
            if (MTL4::RenderCommandEncoder* open = list.encoders.renderEncoder(); open != nullptr) [[likely]]
            {
                return open;
            }
            return openRender(list);
        }

        // `context` is the `DeviceCore`. The arena calls it under its lock while it claims a page.
        Result<core::ArenaPage> createArenaPage(void* context, uint64_t size);
    } // namespace metal4
} // namespace rojoRHI
