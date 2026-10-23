//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12CaptureTests.cpp
/// @brief Tests that the D3D12 backend loads the PIX GPU capturer only on request and captures only on such a device.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "D3D12Capture.h"
#include "D3D12Common.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The capturer's module name, which identifies it once loaded.
    constexpr const wchar_t* kCapturerModule = L"WinPixGpuCapturer.dll";

    /// Owns a device created from a description and destroys it when the case ends, however it ends.
    class OwnedDevice
    {
      public:
        explicit OwnedDevice(const DeviceDesc&);
        ~OwnedDevice();

        //==============================================================================================================
        OwnedDevice(const OwnedDevice&) = delete;

        //==============================================================================================================
        OwnedDevice& operator=(const OwnedDevice&) = delete;

        /// Returns the device; the creation is required to have succeeded.
        Device get() const;

      private:
        Result<Device> m_created; ///< The result of the creation.
    };

    //==================================================================================================================
    OwnedDevice::OwnedDevice(const DeviceDesc& desc) : m_created(createDevice(desc))
    {
    }

    //==================================================================================================================
    OwnedDevice::~OwnedDevice()
    {
        if (m_created.has_value())
        {
            destroyDevice(*m_created);
        }
    }

    //==================================================================================================================
    Device OwnedDevice::get() const
    {
        REQUIRE(m_created.has_value());
        return *m_created;
    }

    //==================================================================================================================
    /// Returns true when the PIX GPU capturer is loaded into the process.
    bool capturerLoaded()
    {
        return GetModuleHandleW(kCapturerModule) != nullptr;
    }

    //==================================================================================================================
    /// Checks that `device`, created without `DeviceDesc::enableCapture`, refuses `beginCapture` naming `enableCapture`,
    /// twice, so that a refusal leaves no capture in progress.
    void checkRefusesWithoutEnableCapture(Device device)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            Result<void> started = device.beginCapture("rojorhi-capture.wpix");
            REQUIRE_FALSE(started.has_value());
            CHECK(started.error().category == ErrorCategory::Unsupported);
            CHECK(started.error().message.find("without DeviceDesc::enableCapture") != std::string::npos);
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("a device without enableCapture loads no capturer and refuses capture", "[d3d12][capture]")
{
    // Whether the capturer was loaded can be checked only when no earlier case loaded it; the refusal holds either way.
    const bool loadedBefore = capturerLoaded();
    const OwnedDevice device(DeviceDesc{});
    Device created = device.get();
    CHECK(capturerLoaded() == loadedBefore);
    CHECK_FALSE(created.capabilities().capture);
    checkRefusesWithoutEnableCapture(created);
}

//======================================================================================================================
TEST_CASE("a device without enableCapture refuses capture beside a device with it", "[d3d12][capture]")
{
    DeviceDesc enabled;
    enabled.enableCapture = true;
    const OwnedDevice capturing(enabled);
    const OwnedDevice plain(DeviceDesc{});
    // A device with enableCapture reports whether the capturer loaded; hosted WARP has no PIX installed, so false there.
    // WARP checks only the false path; hardware with PIX installed is the real check of the true one.
    CHECK(capturing.get().capabilities().capture == d3d12::loadCapturer());
    CHECK_FALSE(plain.get().capabilities().capture);
    // The capturer, when the first device loaded it, is process-wide; the second device still refuses.
    checkRefusesWithoutEnableCapture(plain.get());
}

//======================================================================================================================
TEST_CASE("without PIX, beginCapture returns Unsupported and starts nothing", "[d3d12][capture]")
{
    DeviceDesc desc;
    desc.enableCapture = true;
    const OwnedDevice device(desc);
    Device created = device.get();
    if (capturerLoaded())
    {
        SKIP("this machine has PIX installed");
    }
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        Result<void> started = created.beginCapture("rojorhi-capture.wpix");
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().category == ErrorCategory::Unsupported);
        CHECK(started.error().message.find("PIX") != std::string::npos);
    }
}

//======================================================================================================================
TEST_CASE("the capturer search picks the highest version directory", "[d3d12][capture]")
{
    const std::vector<std::wstring> names = { L"2409.23", L"2501.30", L"2412.1" };
    const std::optional<size_t> newest = d3d12::newestVersion(names);
    REQUIRE(newest.has_value());
    CHECK(names[*newest] == L"2501.30");
    CHECK_FALSE(d3d12::newestVersion(std::vector<std::wstring>{}).has_value());
}
