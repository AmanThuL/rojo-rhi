//----------------------------------------------------------------------------------------------------------------------
/// @file DeviceCore.cpp
/// @brief Implements the backend-neutral device state and the checks of the device chapter.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/DeviceCore.h"

#include "Base/Assert.h"
#include "Base/Log.h"
#include "Core/CommandLists.h"
#include "Core/Label.h"
#include "Core/Swapchains.h"
#include "Core/Tracker/Tracker.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        /// Returns whether releasing `record` releases a native object. A swapchain image's record stands for a drawable the
        /// backend never created, so its retirement is a deferred destruction but no native release.
        template<typename Record> bool releasesNative(const Record&)
        {
            return true;
        }

        //==============================================================================================================
        /// Returns whether releasing the texture `record` releases a native object.
        bool releasesNative(const TextureRecord& record)
        {
            return !record.swapchainImage;
        }

        //==============================================================================================================
        /// Passes a view or sampler slot's payload to `hook`, which may be null.
        void releaseSlotPayload(void (*hook)(void* context, void* native), void* context, void* native)
        {
            if (hook != nullptr)
            {
                hook(context, native);
            }
        }
    } // namespace

    //==================================================================================================================
    Result<void> validateDeviceDesc([[maybe_unused]] const DeviceDesc& desc)
    {
#ifdef NDEBUG
        if (desc.validation)
        {
            return std::unexpected(Error{ ErrorCategory::Unsupported, "DeviceDesc::validation is set, but this Release build contains no validation tracker" });
        }
#endif
        ROJORHI_VU(desc.framesInFlight == 2 || desc.framesInFlight == 3, "VU-rojo-device-0001", "framesInFlight is {}, not 2 or 3", desc.framesInFlight);
        return {};
    }

    //==================================================================================================================
    void checkObject([[maybe_unused]] const void* impl, [[maybe_unused]] std::string_view kind)
    {
        ROJORHI_VU(impl != nullptr, "VU-rojo-core-0004", "a default-constructed {} was used", kind);
    }

    //==================================================================================================================
    DeviceCore::DeviceCore(const DeviceDesc& desc, uint32_t poolCapacity)
        : m_pools(
              PoolCapacity{ poolCapacity },
              PoolCapacity{ poolCapacity },
              PoolCapacity{ poolCapacity },
              PoolCapacity{ poolCapacity },
              PoolCapacity{ poolCapacity },
              PoolCapacity{ poolCapacity },
              PoolCapacity{ poolCapacity }
          ),
          m_framesInFlight(desc.framesInFlight), m_validation(desc.validation), m_label(resolveLabel(desc.label, "rojorhi.device.unnamed")),
          m_scopes(desc.framesInFlight), m_arena(desc.framesInFlight)
    {
        std::get<HandlePool<Buffer, BufferRecord>>(m_pools).countInto(&m_counts.nativeCreateCalls, &m_counts.gpuAllocations);
        std::get<HandlePool<Texture, TextureRecord>>(m_pools).countInto(&m_counts.nativeCreateCalls, &m_counts.gpuAllocations);
        std::get<HandlePool<Heap, HeapRecord>>(m_pools).countInto(&m_counts.nativeCreateCalls, &m_counts.gpuAllocations);
        std::get<HandlePool<Pipeline, PipelineRecord>>(m_pools).countInto(&m_counts.nativeCreateCalls, &m_counts.pipelinesCreated);
        std::get<HandlePool<ShaderLibrary, ShaderLibraryRecord>>(m_pools).countInto(&m_counts.nativeCreateCalls);
        std::get<HandlePool<Fence, FenceRecord>>(m_pools).countInto(&m_counts.nativeCreateCalls);
        std::get<HandlePool<Upscaler, UpscalerRecord>>(m_pools).countInto(&m_counts.nativeCreateCalls);
    }

    //==================================================================================================================
    void DeviceCore::beginFrame()
    {
        const uint64_t state = m_state.load();
        ROJORHI_VU((state & 1) == 0, "VU-rojo-device-0003", "beginFrame was called while frame {} is still open", state >> 1);
        const uint64_t frame = (state >> 1) + 1;
        if (frame > m_framesInFlight && m_hooks.waitFrame != nullptr)
        {
            m_hooks.waitFrame(m_hooks.context, frame - m_framesInFlight);
        }
        m_state.store((frame << 1) | 1);
        m_scopes.clearReport();
        // A backend's wait for the reused slot's frame has returned by now, and a backend with no wait completed the work at
        // submission, so the retired slot's work is complete when the host clock records it. The submissions from here on
        // are the new frame's. No pool lock is held here.
        beginFrameClock(*this, frame > m_framesInFlight ? frame - m_framesInFlight : 0);
        if (frame > m_framesInFlight)
        {
            const uint64_t retired = frame - m_framesInFlight;
            forEachPool([&](auto& pool) {
                const auto hook = m_hooks.forType<typename std::remove_reference_t<decltype(pool)>::HandleType>();
                pool.retire(retired, [&](auto& record) {
                    if (hook != nullptr)
                    {
                        hook(m_hooks.context, record);
                    }
                    DeviceCounts::add(m_counts.deferredRetired);
                    if (releasesNative(record))
                    {
                        DeviceCounts::add(m_counts.nativeCreateCalls);
                    }
                });
            });
            // A destroyed view or sampler is a native object released now and counted; the payloads a released range or a
            // replaced write leaves go to the hooks uncounted.
            const uint64_t heapReleases = m_views.retire(retired, [&](void* native) { releaseSlotPayload(m_hooks.view, m_hooks.context, native); }) +
                                          m_samplers.retire(retired, [&](void* native) { releaseSlotPayload(m_hooks.sampler, m_hooks.context, native); });
            DeviceCounts::add(m_counts.deferredRetired, heapReleases);
            DeviceCounts::add(m_counts.nativeCreateCalls, heapReleases);
            const uint32_t slot = static_cast<uint32_t>(frame % m_framesInFlight);
            // The slot's scopes leave the arena pages their labels live in before the rewind makes the pages claimable.
            m_scopes.retire(slot, retired);
            m_arena.rewind(slot);
            if (m_hooks.frameSlot != nullptr)
            {
                m_hooks.frameSlot(m_hooks.context, slot);
            }
        }
    }

    //==================================================================================================================
    void DeviceCore::endFrame()
    {
        const uint64_t state = m_state.load();
        ROJORHI_VU((state & 1) != 0, "VU-rojo-device-0004", "endFrame was called with no frame open");
        ROJORHI_VU(
            m_unsubmittedLists.load() == 0,
            "VU-rojo-device-0005",
            "endFrame was called with {} command lists of frame {} not submitted",
            m_unsubmittedLists.load(),
            state >> 1
        );
        // The slot's internal fence is signalled on the graphics queue after the frame's graphics submissions, which must have
        // joined the frame's compute and copy work (VU-rojo-device-0006).
        endFrameClock(*this, state >> 1);
        m_state.store(state & ~uint64_t{ 1 });
    }

    //==================================================================================================================
    void DeviceCore::beginDestroy()
    {
        ROJORHI_VU(!frameOpen(), "VU-rojo-device-0002", "destroyDevice was called while frame {} is open", frameNumber());
        // The arena's pages are the library's own buffers, not leaks of the caller.
        for (const Buffer page : m_arena.takePages())
        {
            destroy(page);
        }
        if (m_validation)
        {
            std::apply(
                [](auto&... pools) {
                    (pools.forEachLive([](const auto& record) {
                        // An acquired swapchain image is the swapchain's: the caller cannot destroy it, and the swapchain warns.
                        if constexpr (requires { record.swapchainImage; })
                        {
                            if (record.swapchainImage)
                            {
                                return;
                            }
                        }
                        ROJORHI_LOG_WARN("object \"{}\" is still live at device destruction", record.label);
                    }),
                     ...);
                },
                m_pools
            );
        }
        {
            std::lock_guard lock(m_swapchainMutex);
            if (m_validation)
            {
                for (const SwapchainCore* swapchain : m_swapchains)
                {
                    ROJORHI_LOG_WARN("object \"{}\" is still live at device destruction", swapchain->label);
                }
            }
            m_swapchains.clear();
        }
        drain();
        m_views.drain([&](void* native) { releaseSlotPayload(m_hooks.view, m_hooks.context, native); });
        m_samplers.drain([&](void* native) { releaseSlotPayload(m_hooks.sampler, m_hooks.context, native); });
    }

    //==================================================================================================================
    void DeviceCore::drain()
    {
        forEachPool([&](auto& pool) {
            const auto hook = m_hooks.forType<typename std::remove_reference_t<decltype(pool)>::HandleType>();
            pool.drain([&](auto& record) {
                if (hook != nullptr)
                {
                    hook(m_hooks.context, record);
                }
            });
        });
    }

    //==================================================================================================================
    bool DeviceCore::drained()
    {
        bool empty = true;
        forEachPool([&](auto& pool) { empty = pool.empty() && empty; });
        return empty && m_views.empty() && m_samplers.empty();
    }

    //==================================================================================================================
    DeviceCore::~DeviceCore()
    {
        ROJORHI_INTERNAL_ASSERT(drained(), "a device core was destroyed with live or pending records; its backend must call beginDestroy first");
        // Release builds only clear: a hook would run on backend state that is already gone.
        forEachPool([](auto& pool) { pool.drain([](auto&) {}); });
    }

    //==================================================================================================================
    void DeviceCore::setRetireHooks(const RetireHooks& hooks)
    {
        m_hooks = hooks;
    }

    //==================================================================================================================
    const RetireHooks& DeviceCore::retireHooks() const
    {
        return m_hooks;
    }

    //==================================================================================================================
    void DeviceCore::setSurfaceCheck(bool (*check)(const void* surface))
    {
        m_surfaceCheck = check;
    }

    //==================================================================================================================
    bool (*DeviceCore::surfaceCheck() const)(const void* surface)
    {
        return m_surfaceCheck;
    }

    //==================================================================================================================
    void DeviceCore::setArenaSource(ArenaPageSource source)
    {
        m_arenaSource = source;
    }

    //==================================================================================================================
    ArenaPageSource DeviceCore::arenaSource() const
    {
        ROJORHI_INTERNAL_ASSERT(m_arenaSource != nullptr, "the backend registered no frame-arena page source");
        return m_arenaSource;
    }

    //==================================================================================================================
    void DeviceCore::setCaptureAvailable(Result<void> (*available)(std::string_view path))
    {
        m_captureAvailable = available;
    }

    //==================================================================================================================
    Result<void> (*DeviceCore::captureAvailable() const)(std::string_view path)
    {
        return m_captureAvailable;
    }

    //==================================================================================================================
    bool DeviceCore::capturing() const
    {
        return m_capturing.load();
    }

    //==================================================================================================================
    bool DeviceCore::exchangeCapturing(bool capturing)
    {
        return m_capturing.exchange(capturing);
    }

    //==================================================================================================================
    void DeviceCore::submitScopes(const CommandListCore& list)
    {
        m_scopes.submit(static_cast<uint32_t>(list.frame % m_framesInFlight), list.type, list.scopeLog);
    }

    //==================================================================================================================
    std::span<const ScopeTiming> DeviceCore::scopeTimings() const
    {
        const std::span<const ScopeTiming> report = m_scopes.report();
        if (!report.empty())
        {
            // The backend reads the timestamps of the retired frame's scopes from its counter heap.
            DeviceCounts::add(m_counts.nativeHostCalls);
        }
        return report;
    }

    //==================================================================================================================
    DeviceCounts& DeviceCore::counts() const
    {
        return m_counts;
    }

    //==================================================================================================================
    Counters DeviceCore::counters() const
    {
        Counters counters;
        // Every count is its own atomic, so a snapshot is plain relaxed loads and takes no lock; only the misses are a
        // difference of two reads.
        counters.nativeCreateCalls = DeviceCounts::read(m_counts.nativeCreateCalls);
        counters.nativeRecordCalls = DeviceCounts::read(m_counts.nativeRecordCalls);
        counters.nativeSubmitCalls = DeviceCounts::read(m_counts.nativeSubmitCalls);
        counters.nativeHostCalls = DeviceCounts::read(m_counts.nativeHostCalls);
        counters.cpuAllocations = DeviceCounts::read(m_counts.cpuAllocations);
        // Frame-arena pages are buffers of the pool, so they are counted with the buffers.
        counters.gpuAllocations = DeviceCounts::read(m_counts.gpuAllocations);
        counters.frameDataUploads = DeviceCounts::read(m_counts.frameDataUploads);
        counters.frameArenaBytes = DeviceCounts::read(m_counts.frameArenaBytes);
        counters.frameArenaGrowths = m_arena.growths();
        counters.descriptorWrites = DeviceCounts::read(m_counts.descriptorWrites);
        counters.globalBarriers = DeviceCounts::read(m_counts.globalBarriers);
        counters.bufferBarriers = DeviceCounts::read(m_counts.bufferBarriers);
        counters.textureBarriers = DeviceCounts::read(m_counts.textureBarriers);
        counters.nativeBarriers = DeviceCounts::read(m_counts.nativeBarriers);
        counters.pipelinesCreated = DeviceCounts::read(m_counts.pipelinesCreated);
        // A creation the backend's cache did not serve is a miss; the Null backend never counts a hit. The two counts are read
        // apart: a hit whose creation lands after the creations are read but before the hits are shows, in that one
        // snapshot, as an earlier miss turned into a hit (or, with no earlier miss, would wrap, hence the clamp at zero).
        // The next snapshot after the creation returns is exact.
        counters.pipelineCacheHits = DeviceCounts::read(m_counts.pipelineCacheHits);
        counters.pipelineCacheMisses = counters.pipelinesCreated - std::min(counters.pipelineCacheHits, counters.pipelinesCreated);
        counters.pipelineCompileNanoseconds = DeviceCounts::read(m_counts.pipelineCompileNanoseconds);
        counters.encodersOpened = DeviceCounts::read(m_counts.encodersOpened);
        counters.deferredDestructionsRetired = DeviceCounts::read(m_counts.deferredRetired);
        return counters;
    }

    //==================================================================================================================
    void DeviceCore::addSwapchain(SwapchainCore& swapchain)
    {
        std::lock_guard lock(m_swapchainMutex);
        ROJORHI_VU(
            std::none_of(m_swapchains.begin(), m_swapchains.end(), [&](const SwapchainCore* live) { return live->surface == swapchain.surface; }),
            "VU-rojo-present-0006",
            "the surface of the swapchain \"{}\" is the surface of another swapchain of the device that was not destroyed",
            swapchain.label
        );
        m_swapchains.push_back(&swapchain);
    }

    //==================================================================================================================
    void DeviceCore::removeSwapchain(SwapchainCore& swapchain)
    {
        std::lock_guard lock(m_swapchainMutex);
        std::erase(m_swapchains, &swapchain);
    }

    //==================================================================================================================
    Capabilities& DeviceCore::capabilities()
    {
        return m_capabilities;
    }

    //==================================================================================================================
    const Capabilities& DeviceCore::capabilities() const
    {
        return m_capabilities;
    }

    //==================================================================================================================
    void DeviceCore::setFormatCapabilities(Format format, const FormatCapabilities& capabilities)
    {
        ROJORHI_INTERNAL_ASSERT(static_cast<uint32_t>(format) < kFormatCount, "a backend set capabilities for a value that is not a Format");
        m_formats[static_cast<uint32_t>(format)] = capabilities;
    }

    //==================================================================================================================
    void DeviceCore::setFormatTable(const FormatTable& table)
    {
        m_formats = table;
    }

    //==================================================================================================================
    void DeviceCore::setTileExtents(TileExtent (*extents)(Format format, TextureType type))
    {
        m_tileExtents = extents;
    }

    //==================================================================================================================
    TileExtent DeviceCore::tileExtent(Format format, TextureType type) const
    {
        ROJORHI_INTERNAL_ASSERT(m_tileExtents != nullptr, "the backend registered no tile extents");
        return m_tileExtents(format, type);
    }

    //==================================================================================================================
    FormatCapabilities DeviceCore::formatCapabilities(Format format) const
    {
        checkQueryFormat(format);
        return m_formats[static_cast<uint32_t>(format)];
    }

    //==================================================================================================================
    bool DeviceCore::frameOpen() const
    {
        return (m_state.load() & 1) != 0;
    }

    //==================================================================================================================
    uint64_t DeviceCore::frameNumber() const
    {
        return m_state.load() >> 1;
    }

    //==================================================================================================================
    uint64_t DeviceCore::releaseFrame() const
    {
        const uint64_t state = m_state.load();
        return (state & 1) != 0 ? (state >> 1) : (state >> 1) + 1;
    }

    //==================================================================================================================
    SlotAllocator& DeviceCore::views()
    {
        return m_views;
    }

    //==================================================================================================================
    SlotAllocator& DeviceCore::samplers()
    {
        return m_samplers;
    }

    //==================================================================================================================
    FrameArena& DeviceCore::arena()
    {
        return m_arena;
    }

    //==================================================================================================================
    void DeviceCore::noteListAcquired()
    {
        ++m_unsubmittedLists;
    }

    //==================================================================================================================
    void DeviceCore::noteListSubmitted()
    {
        --m_unsubmittedLists;
    }

    //==================================================================================================================
    uint32_t DeviceCore::framesInFlight() const
    {
        return m_framesInFlight;
    }

    //==================================================================================================================
    bool DeviceCore::validation() const
    {
        return m_validation;
    }

    //==================================================================================================================
    Tracker& DeviceCore::tracker()
    {
        return m_tracker;
    }

    //==================================================================================================================
    std::string_view DeviceCore::label() const
    {
        return m_label;
    }
} // namespace rojoRHI::core
