//----------------------------------------------------------------------------------------------------------------------
/// @file DeviceCore.h
/// @brief Declares the backend-neutral device state, its frame bookkeeping and the checks of the device chapter.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/AliasTracker.h"
#include "Core/Counters.h"
#include "Core/Formats.h"
#include "Core/FrameArena.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Core/ScopeLog.h"
#include "Core/SlotAllocator.h"
#include "Core/Tracker/Tracker.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace rojoRHI::core {
    struct SwapchainCore;
    struct CommandListCore;
    class DeviceCore;

    // Fatal when an unsubmitted list references `handle`; defined in Tracker/References.cpp for every pooled handle type.
    template<typename Handle> void checkUnreferencedChecked(DeviceCore& core, Handle handle);

    // `ErrorCategory::Unsupported` when validation is requested in a Release build. A backend calls it before it opens
    // anything.
    Result<void> validateDeviceDesc(const DeviceDesc& desc);

    void checkObject(const void* impl, std::string_view kind);

    // Frames are numbered from one; frame N uses slot N modulo `framesInFlight`, and a slot's deferred destructions are
    // released when frame N + `framesInFlight` begins.
    class DeviceCore
    {
      public:
        // Resolves the label and opens no frame. `poolCapacity` counts slot zero; a test passes a small value to fill a pool.
        explicit DeviceCore(const DeviceDesc& desc, uint32_t poolCapacity = NativeTable::kCapacity);

        // Calls no hook: `beginDestroy` drained every pool and the backend's state is already destroyed.
        ~DeviceCore();

        DeviceCore(const DeviceCore&) = delete;
        DeviceCore& operator=(const DeviceCore&) = delete;

        // Called once at creation, before anything is allocated.
        void setRetireHooks(const RetireHooks& hooks);

        const RetireHooks& retireHooks() const;

        // Tells a surface of the backend's type from any other pointer (VU-rojo-present-0002); with none, every surface
        // is accepted.
        void setSurfaceCheck(bool (*check)(const void* surface));

        bool (*surfaceCheck() const)(const void* surface);

        // `swapchain` must be at its final address. Free-threaded.
        void addSwapchain(SwapchainCore& swapchain);

        // Free-threaded; the surface may take a new swapchain at once.
        void removeSwapchain(SwapchainCore& swapchain);

        // The core copies scope labels into the frame arena through the backend's page source.
        void setArenaSource(ArenaPageSource source);

        ArenaPageSource arenaSource() const;

        // The hook fails when the capture tool is unavailable or the destination cannot be written; with none,
        // `Device::beginCapture` returns `ErrorCategory::Unsupported`.
        void setCaptureAvailable(Result<void> (*available)(std::string_view path));

        Result<void> (*captureAvailable() const)(std::string_view path);

        bool capturing() const;

        bool exchangeCapturing(bool capturing);

        // Does nothing on a device that times no scope. Free-threaded.
        void submitScopes(const CommandListCore& list);

        // The frame the latest `beginFrame` waited for; the next `beginFrame` invalidates the span. The backend counts
        // the native reads, not this.
        std::span<const ScopeTiming> scopeTimings() const;

        // Relaxed loads. Free-threaded.
        Counters counters() const;

        DeviceCounts& counts() const;

        // A backend fills them in place at creation; their views must point at storage that outlives the device.
        Capabilities& capabilities();

        const Capabilities& capabilities() const;

        void setFormatCapabilities(Format format, const FormatCapabilities& capabilities);

        void setFormatTable(const FormatTable& table);

        FormatCapabilities formatCapabilities(Format format) const;

        void setTileExtents(TileExtent (*extents)(Format format, TextureType type));

        TileExtent tileExtent(Format format, TextureType type) const;

        // Reusing a slot first waits, through `waitFrame`, for the frame that last used it, then retires what the slot held.
        // Frame class.
        void beginFrame();

        // Returns the count before `mips` (VU-rojo-cmd-0073).
        uint32_t addStorageClears(uint32_t mips);

        // Frame class.
        void endFrame();

        // Warns of each live object when validating, then retires every live and pending record and every heap payload at
        // once. The backend has already waited for all submitted work, as `destroyDevice` requires.
        void beginDestroy();

        // Deferred to the retirement of the open frame, or of the next one if none is open. Free-threaded.
        template<typename Handle> void destroy(Handle handle)
        {
#ifndef NDEBUG
            checkUnreferencedChecked(*this, handle);
#endif
            pool<Handle>().checkedRelease(handle, [this] { return releaseFrame(); });
        }

        bool frameOpen() const;

        // The open frame, or the last one when none is open; zero before the first.
        uint64_t frameNumber() const;

        // The open frame, else the next one to open.
        uint64_t releaseFrame() const;

        template<typename Handle> HandlePool<Handle, RecordFor<Handle>>& pool()
        {
            return std::get<HandlePool<Handle, RecordFor<Handle>>>(m_pools);
        }

        // A slot released from either heap retires with the frame that released it.
        SlotAllocator& views();

        SlotAllocator& samplers();

        // `beginFrame` rewinds a slot's pages when it reuses the slot.
        FrameArena& arena();

        // `endFrame` requires every list acquired in the open frame to be submitted (VU-rojo-device-0005).
        // Free-threaded.
        void noteListAcquired();

        // Free-threaded.
        void noteListSubmitted();

        uint32_t framesInFlight() const;

        bool validation() const;

        // A backend writes scope timestamps, and claims storage for them, only when this is true.
        bool timesScopes() const;

        // Set by `DeviceDesc::debugMarkers` or `enableCapture` (ADR 0034). Without it a backend sends no debug group,
        // event or label to the driver; the core still records every scope and its label.
        bool marksScopes() const;

        // Filled only on a device created with validation, in a Debug build.
        Tracker& tracker();

        AliasTracker& aliasing();

        const AliasTracker& aliasing() const;

        std::string_view label() const;

      private:
        // One visit per pool, so a handle type added to `m_pools` is never skipped.
        template<typename Visitor> void forEachPool(Visitor&& visit);

        void drain();

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
        TileExtent (*m_tileExtents)(Format format, TextureType type) = nullptr;
        ArenaPageSource m_arenaSource = nullptr;
        Result<void> (*m_captureAvailable)(std::string_view path) = nullptr;
        std::atomic<bool> m_capturing = false;
        // VU-rojo-cmd-0073's count for the open frame; written only in Debug builds.
        std::atomic<uint32_t> m_storageClears = 0;
        uint32_t m_framesInFlight;
        bool m_validation;
        bool m_timesScopes;
        bool m_marksScopes;
        std::string m_label;
        ScopeLog m_scopes;
        FrameArena m_arena;
        Tracker m_tracker;
        AliasTracker m_aliasing;
        // Mutable: reading a retired frame's timings, a const call, counts a native read.
        mutable DeviceCounts m_counts;
        // Frame number shifted left by one, bit zero set while it is open: one word, so the free-threaded destroy reads
        // a consistent pair while the frame class writes it.
        std::atomic<uint64_t> m_state = 0;
    };

    template<typename Visitor> void DeviceCore::forEachPool(Visitor&& visit)
    {
        std::apply([&](auto&... pools) { (visit(pools), ...); }, m_pools);
    }

    // Each backend defines it; the shared checks reach a device's state only through it.
    DeviceCore& coreOf(Device device);
} // namespace rojoRHI::core
