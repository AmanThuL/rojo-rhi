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

#include <algorithm>
#include <type_traits>
#include <utility>

namespace rojoRHI::core {
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
    DeviceCore::DeviceCore(const DeviceDesc& desc)
        : m_framesInFlight(desc.framesInFlight), m_validation(desc.validation), m_label(resolveLabel(desc.label, "rojorhi.device.unnamed")),
          m_scopes(desc.framesInFlight), m_arena(desc.framesInFlight)
    {
    }

    //==================================================================================================================
    void DeviceCore::beginFrame()
    {
        const uint64_t state = m_state.load();
        ROJORHI_VU((state & 1) == 0, "VU-rojo-device-0003", "beginFrame was called while frame {} is still open", state >> 1);
        const uint64_t frame = (state >> 1) + 1;
        m_state.store((frame << 1) | 1);
        m_scopes.clearReport();
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
                });
            });
            m_views.retire(retired);
            m_samplers.retire(retired);
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
                    (pools.forEachLive([](const auto& record) { ROJORHI_LOG_WARN("object \"{}\" is still live at device destruction", record.label); }), ...);
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
        return empty;
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
        return m_scopes.report();
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
    std::string_view DeviceCore::label() const
    {
        return m_label;
    }
} // namespace rojoRHI::core
