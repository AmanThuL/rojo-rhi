//----------------------------------------------------------------------------------------------------------------------
/// @file DeviceCore.h
/// @brief Declares the backend-neutral device state, its frame bookkeeping and the checks of the device chapter.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Capabilities.h>
#include <rojoRHI/Device.h>
#include <rojoRHI/DeviceDesc.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/Result.h>

#include "Core/Formats.h"
#include "Core/FrameArena.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Core/SlotAllocator.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace rojoRHI::core {
    struct SwapchainCore;

    /// Checks a `DeviceDesc` before a backend opens anything: `ErrorCategory::Unsupported` when validation is requested in a
    /// Release build, and VU-rojo-device-0001 for a frame-slot count other than 2 or 3. A backend calls it first in
    /// `createDevice`.
    Result<void> validateDeviceDesc(const DeviceDesc& desc);

    /// Issues VU-rojo-core-0004 when `impl`, the pointer an object handle wraps, is null; `kind` names the object type.
    void checkObject(const void* impl, std::string_view kind);

    /// The state every backend's device shares. A backend's `DeviceImpl` embeds it, so the frame rules and handle checks live
    /// here once. Frames are numbered from one; frame N uses slot N modulo `framesInFlight`, and a slot's deferred
    /// destructions are released when frame N + `framesInFlight` begins.
    class DeviceCore
    {
      public:
        /// Creates the core of a device from a description `validateDeviceDesc` accepted: it resolves the label (VU-rojo-core-0006,
        /// VU-rojo-core-0007), records `validation` and opens no frame.
        explicit DeviceCore(const DeviceDesc& desc);

        /// Requires that `beginDestroy` drained every pool, and calls no hook: the backend's state is already destroyed.
        ~DeviceCore();

        DeviceCore(const DeviceCore&) = delete;
        DeviceCore& operator=(const DeviceCore&) = delete;

        /// Registers the functions that release a backend's payload of each record leaving a pool. A backend calls it once
        /// at creation, before it allocates anything.
        void setRetireHooks(const RetireHooks& hooks);

        /// Returns the hooks registered for retiring records, so a test can wrap them.
        const RetireHooks& retireHooks() const;

        /// Registers the predicate that tells a surface of the compiled backend's type from any other pointer
        /// (VU-rojo-present-0002); a backend whose surface has a recognizable type calls it once at creation. Without one every
        /// surface is accepted.
        void setSurfaceCheck(bool (*check)(const void* surface));

        /// Returns the registered surface predicate, or null when the backend registered none.
        bool (*surfaceCheck() const)(const void* surface);

        /// Adds `swapchain`, whose record is at its final address, to the live set, after checking that no live swapchain of the
        /// device holds its surface (VU-rojo-present-0006). Free-threaded.
        void addSwapchain(SwapchainCore& swapchain);

        /// Removes `swapchain` from the live set; its surface may take a new swapchain at once. Free-threaded.
        void removeSwapchain(SwapchainCore& swapchain);

        /// Returns the capabilities the device reports; a backend fills them in place at creation, and its views must point at
        /// storage that outlives the device.
        Capabilities& capabilities();

        /// Returns the capabilities the device reports.
        const Capabilities& capabilities() const;

        /// Sets what the device supports for `format`; a backend calls it at creation for each format it reports.
        void setFormatCapabilities(Format format, const FormatCapabilities& capabilities);

        /// Sets the whole format table at once; a backend calls it at creation.
        void setFormatTable(const FormatTable& table);

        /// Returns what the device supports for `format` (VU-rojo-format-0001, VU-rojo-format-0005).
        FormatCapabilities formatCapabilities(Format format) const;

        /// Opens the next frame and passes what the slot it reuses held to the retire hooks (VU-rojo-device-0003). Frame class.
        void beginFrame();

        /// Closes the open frame (VU-rojo-device-0004, VU-rojo-device-0005). Frame class.
        void endFrame();

        /// Starts teardown: VU-rojo-device-0002, then, when validation was selected, a warning for each live object, then every
        /// live and pending record goes through the retire hooks once.
        void beginDestroy();

        /// Destroys the object `handle` names, deferred to the retirement of the open frame, or of the next one opened if none
        /// is (VU-rojo-core-0001 to 0003). Free-threaded.
        template<typename Handle> void destroy(Handle handle)
        {
            pool<Handle>().checkedRelease(handle, [this] { return releaseFrame(); });
        }

        /// Returns true between a beginFrame and its endFrame.
        bool frameOpen() const;

        /// Returns the number of the open frame, or of the last one when none is open; zero before the first.
        uint64_t frameNumber() const;

        /// Returns the frame an object destroyed now is released in: the open frame, else the next one to open.
        uint64_t releaseFrame() const;

        /// Returns the pool that issues and validates handles of type `Handle`.
        template<typename Handle> HandlePool<Handle, RecordFor<Handle>>& pool()
        {
            return std::get<HandlePool<Handle, RecordFor<Handle>>>(m_pools);
        }

        /// Returns the allocator of the global view heap; `Device::createView` takes its slots and `destroy(ViewIndex)` releases
        /// them, and `beginFrame` retires them with the frame that released them.
        SlotAllocator& views();

        /// Returns the allocator of the global sampler heap; `Device::createSampler` takes its slots and `destroy(SamplerIndex)`
        /// releases them, and `beginFrame` retires them with the frame that released them.
        SlotAllocator& samplers();

        /// Returns the arena `CommandList::uploadFrameData` suballocates from; `beginFrame` rewinds a slot's pages when it
        /// reuses the slot, and `beginDestroy` destroys them.
        FrameArena& arena();

        /// Counts a command list acquired in the open frame, which `endFrame` requires to be submitted
        /// (VU-rojo-device-0005). Free-threaded.
        void noteListAcquired();

        /// Counts a command list of the open frame as submitted. Free-threaded.
        void noteListSubmitted();

        /// Returns the number of frame slots.
        uint32_t framesInFlight() const;

        /// Returns true when the device was created with the validation tracker selected.
        bool validation() const;

        /// Returns the device's label.
        std::string_view label() const;

      private:
        /// Calls `visit` with each pool, so a handle type added to `m_pools` is never skipped.
        template<typename Visitor> void forEachPool(Visitor&& visit);

        /// Passes every live and pending record to its hook, once.
        void drain();

        /// Returns true when no pool holds a live or pending record.
        bool drained();

        std::tuple<
            HandlePool<Buffer, BufferRecord>,
            HandlePool<Texture, TextureRecord>,
            HandlePool<Pipeline, PipelineRecord>,
            HandlePool<ShaderLibrary, ShaderLibraryRecord>,
            HandlePool<Heap, HeapRecord>,
            HandlePool<Fence, FenceRecord>,
            HandlePool<Upscaler, UpscalerRecord>>
            m_pools;
        SlotAllocator m_views;
        SlotAllocator m_samplers;
        RetireHooks m_hooks;
        Capabilities m_capabilities;
        FormatTable m_formats;
        std::atomic<uint64_t> m_unsubmittedLists = 0;
        std::mutex m_swapchainMutex;
        std::vector<SwapchainCore*> m_swapchains;
        bool (*m_surfaceCheck)(const void* surface) = nullptr;
        uint32_t m_framesInFlight;
        bool m_validation;
        std::string m_label;
        FrameArena m_arena;
        /// Frame number shifted left by one, with bit zero set while that frame is open; one word so the free-threaded
        /// destroy reads a consistent pair while the frame class writes it.
        std::atomic<uint64_t> m_state = 0;
    };

    template<typename Visitor> void DeviceCore::forEachPool(Visitor&& visit)
    {
        std::apply([&](auto&... pools) { (visit(pools), ...); }, m_pools);
    }

    /// Returns the core of the device `device` wraps (VU-rojo-core-0004). Each backend defines it; the contract tests and
    /// the shared checks reach a device's state only through it.
    DeviceCore& coreOf(Device device);
} // namespace rojoRHI::core
