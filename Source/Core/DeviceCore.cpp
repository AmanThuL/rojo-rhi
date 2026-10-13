//----------------------------------------------------------------------------------------------------------------------
/// @file DeviceCore.cpp
/// @brief Implements the backend-neutral device state and the checks of the device chapter.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/DeviceCore.h"

#include "Base/Assert.h"
#include "Base/Log.h"
#include "Core/Label.h"

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
        : m_framesInFlight(desc.framesInFlight), m_validation(desc.validation), m_label(resolveLabel(desc.label, "rojorhi.device.unnamed"))
    {
    }

    //==================================================================================================================
    void DeviceCore::beginFrame()
    {
        const uint64_t state = m_state.load();
        ROJORHI_VU((state & 1) == 0, "VU-rojo-device-0003", "beginFrame was called while frame {} is still open", state >> 1);
        const uint64_t frame = (state >> 1) + 1;
        m_state.store((frame << 1) | 1);
        if (frame > m_framesInFlight)
        {
            const uint64_t retired = frame - m_framesInFlight;
            std::apply([retired](auto&... pools) { (pools.retire(retired), ...); }, m_pools);
        }
    }

    //==================================================================================================================
    void DeviceCore::endFrame()
    {
        const uint64_t state = m_state.load();
        ROJORHI_VU((state & 1) != 0, "VU-rojo-device-0004", "endFrame was called with no frame open");
        m_state.store(state & ~uint64_t{ 1 });
    }

    //==================================================================================================================
    void DeviceCore::beginDestroy()
    {
        ROJORHI_VU(!frameOpen(), "VU-rojo-device-0002", "destroyDevice was called while frame {} is open", frameNumber());
        if (!m_validation)
        {
            return;
        }
        std::apply(
            [](auto&... pools) {
                (pools.forEachLive([](const ResourceRecord& record) { ROJORHI_LOG_WARN("object \"{}\" is still live at device destruction", record.label); }),
                 ...);
            },
            m_pools
        );
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
