//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12DeviceTests.cpp
/// @brief Tests a D3D12 device against what it opened: the adapter, the Agility runtime, the debug layer, labels and the floor.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "D3D12Capabilities.h"
#include "D3D12Device.h"
#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <string>
#include <string_view>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// Returns the full path of `module`, the running executable for null.
    std::wstring modulePath(HMODULE module)
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0)
            {
                return {};
            }
            if (length < path.size())
            {
                path.resize(length);
                return path;
            }
            path.resize(path.size() * 2);
        }
    }

    //==================================================================================================================
    /// Returns true when `a` and `b` name the same path, ignoring case, as Windows compares file names.
    bool samePath(std::wstring_view a, std::wstring_view b)
    {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    }

    /// True when the build selects the debug layer: `layer` and `gbv` always, `mode` in Debug builds only.
#if defined(ROJORHI_D3D12_DEBUG_LAYER) || defined(ROJORHI_D3D12_DEBUG_GBV) || (defined(ROJORHI_D3D12_DEBUG_MODE) && !defined(NDEBUG))
    constexpr bool kDebugLayer = true;
#else
    constexpr bool kDebugLayer = false;
#endif

    /// True when the build selects the WARP adapter.
#if defined(ROJORHI_D3D12_ADAPTER_WARP)
    constexpr bool kWarpAdapter = true;
#else
    constexpr bool kWarpAdapter = false;
#endif
} // namespace

//======================================================================================================================
TEST_CASE("a device is created on the configured adapter at feature level 12_2", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    REQUIRE(impl.device() != nullptr);
    DXGI_ADAPTER_DESC1 adapter = {};
    REQUIRE(SUCCEEDED(impl.loaded.adapter->GetDesc1(&adapter)));
    const bool software = (adapter.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    CHECK(software == kWarpAdapter);
    const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_12_2;
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
    levels.NumFeatureLevels = 1;
    levels.pFeatureLevelsRequested = &requested;
    REQUIRE(SUCCEEDED(impl.device()->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, static_cast<UINT>(sizeof(levels)))));
    CHECK(levels.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_2);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the Agility runtime loads from the D3D12 directory beside the executable", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    const HMODULE runtime = GetModuleHandleW(L"D3D12Core.dll");
    REQUIRE(runtime != nullptr);
    const std::wstring executable = modulePath(nullptr);
    const size_t slash = executable.find_last_of(L"\\/");
    REQUIRE(slash != std::wstring::npos);
    const std::wstring expected = executable.substr(0, slash) + L"\\D3D12\\D3D12Core.dll";
    const std::wstring loaded = modulePath(runtime);
    INFO("loaded " << d3d12::utf8(loaded) << ", expected " << d3d12::utf8(expected));
    CHECK(samePath(loaded, expected));
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the debug layer is on exactly when the build selects it", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    ID3D12DebugDevice* debug = nullptr;
    const bool layer = SUCCEEDED(impl.device()->QueryInterface(IID_PPV_ARGS(&debug)));
    if (debug != nullptr)
    {
        debug->Release();
    }
    CHECK(layer == kDebugLayer);
    CHECK(impl.loaded.layer == kDebugLayer);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the device carries its label", "[d3d12]")
{
    DeviceDesc desc;
    desc.label = "test.d3d12.device";
    Result<Device> created = createDevice(desc);
    REQUIRE(created.has_value());
    ID3D12Device12* device = DeviceImpl::of(*created).device();
    wchar_t name[64] = {};
    UINT size = static_cast<UINT>(sizeof(name));
    REQUIRE(SUCCEEDED(device->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)));
    // The size counts bytes; a terminating null, when stored, is dropped.
    std::wstring read(name, size / sizeof(wchar_t));
    while (!read.empty() && read.back() == L'\0')
    {
        read.pop_back();
    }
    INFO("read " << d3d12::utf8(read));
    CHECK(read == L"test.d3d12.device");
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("two devices coexist and a third is created after both are destroyed", "[d3d12]")
{
    Result<Device> first = createDevice();
    REQUIRE(first.has_value());
    Result<Device> second = createDevice();
    REQUIRE(second.has_value());
    CHECK(DeviceImpl::of(*first).device() != DeviceImpl::of(*second).device());
    destroyDevice(*first);
    destroyDevice(*second);
    Result<Device> third = createDevice();
    REQUIRE(third.has_value());
    destroyDevice(*third);
}

//======================================================================================================================
TEST_CASE("the floor check accepts the device it created", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    const Result<void> floor = d3d12::checkFloor(DeviceImpl::of(*created).device());
    INFO((floor.has_value() ? std::string() : floor.error().message));
    CHECK(floor.has_value());
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the surface check accepts a test surface and rejects another pointer", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    const auto check = core::coreOf(*created).surfaceCheck();
    REQUIRE(check != nullptr);
    CHECK(check(test::surface(0)));
    const int local = 0;
    CHECK_FALSE(check(&local));
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("capture is unsupported without a capture tool", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    const Result<void> started = created->beginCapture("x.wpix");
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().category == ErrorCategory::Unsupported);
    destroyDevice(*created);
}
