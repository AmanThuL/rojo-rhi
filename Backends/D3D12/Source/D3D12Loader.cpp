//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Loader.cpp
/// @brief Implements how a D3D12 device is opened: the OS check, the Agility device factory, debug features and the adapter.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Loader.h"

#include "D3D12Capabilities.h"
#include "D3D12Common.h"

#include <d3d12sdklayers.h>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI::d3d12 {
    namespace {
        static_assert(D3D12_SDK_VERSION >= 619, "the D3D12 backend needs the Agility SDK headers of version 619 or later");

        /// The Windows build the backend requires: the Windows 11 kernel line, which Windows Server 2025 shares.
        constexpr ULONG kMinimumBuild = 22'000;

        /// The adapter kind the build selects (`rojorhi_d3d12_adapter`): WARP, or the high-performance hardware adapters.
#if defined(ROJORHI_D3D12_ADAPTER_WARP)
        constexpr bool kWarpAdapter = true;
#else
        constexpr bool kWarpAdapter = false;
#endif

        /// The debug features the build selects (`rojorhi_d3d12_debug`): `layer` and `gbv` always, `mode` in Debug builds only,
        /// `off` never. DRED goes with the layer; GPU-based validation only with `gbv`.
#if defined(ROJORHI_D3D12_DEBUG_GBV)
        constexpr bool kDebugLayer = true;
        constexpr bool kGpuValidation = true;
#elif defined(ROJORHI_D3D12_DEBUG_LAYER) || (defined(ROJORHI_D3D12_DEBUG_MODE) && !defined(NDEBUG))
        constexpr bool kDebugLayer = true;
        constexpr bool kGpuValidation = false;
#else
        constexpr bool kDebugLayer = false;
        constexpr bool kGpuValidation = false;
#endif

        //==============================================================================================================
        /// Returns the error of a requirement the process does not meet.
        std::unexpected<Error> missing(std::string message)
        {
            return std::unexpected(Error{ ErrorCategory::Unsupported, std::move(message) });
        }

        //==============================================================================================================
        /// Returns the 32 bits of `result`, which a message prints as eight hexadecimal digits.
        uint32_t hresultBits(HRESULT result)
        {
            return static_cast<uint32_t>(result);
        }

        //==============================================================================================================
        /// Returns the full path of the running executable.
        std::wstring executablePath()
        {
            std::wstring path(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
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

        //==============================================================================================================
        /// Returns `text` in the process code page, the encoding `CreateDeviceFactory` reads its path in.
        std::string narrow(std::wstring_view text)
        {
            if (text.empty())
            {
                return {};
            }
            const int wide = static_cast<int>(text.size());
            const int size = WideCharToMultiByte(CP_ACP, 0, text.data(), wide, nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<size_t>(size > 0 ? size : 0), '\0');
            WideCharToMultiByte(CP_ACP, 0, text.data(), wide, result.data(), size, nullptr, nullptr);
            return result;
        }

        //==============================================================================================================
        /// Returns `text` in UTF-8, for messages.
        std::string utf8(std::wstring_view text)
        {
            if (text.empty())
            {
                return {};
            }
            const int wide = static_cast<int>(text.size());
            const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), wide, nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<size_t>(size > 0 ? size : 0), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), wide, result.data(), size, nullptr, nullptr);
            return result;
        }

        //==============================================================================================================
        /// Refuses a Windows build below the Windows 11 kernel line, read with `RtlGetVersion`, which reports the true build
        /// whatever the executable's compatibility manifest says.
        Result<void> checkWindows()
        {
            using RtlGetVersionFunction = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
            const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            const auto rtlGetVersion = ntdll != nullptr ? reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
            RTL_OSVERSIONINFOW version = {};
            version.dwOSVersionInfoSize = static_cast<ULONG>(sizeof(version));
            if (rtlGetVersion == nullptr || rtlGetVersion(&version) != 0)
            {
                return missing("Windows 11 (build 22000) or later is required, and RtlGetVersion did not report the build");
            }
            if (version.dwBuildNumber < kMinimumBuild)
            {
                return missing(std::format("Windows 11 (build 22000) or later is required; this is build {}", version.dwBuildNumber));
            }
            return {};
        }

        //==============================================================================================================
        /// Creates the device factory of the Agility runtime in the `D3D12\` directory beside the executable. It is the only
        /// way the backend loads the runtime, so a consumer executable exports nothing.
        Result<Com<ID3D12DeviceFactory>> createFactory()
        {
            Com<ID3D12SDKConfiguration1> configuration;
            if (const HRESULT got = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(configuration.put())); FAILED(got))
            {
                return missing(
                    std::format(
                        "the Agility SDK runtime is required: D3D12GetInterface(CLSID_D3D12SDKConfiguration) failed with HRESULT 0x{:08X}", hresultBits(got)
                    )
                );
            }
            const std::wstring executable = executablePath();
            const size_t slash = executable.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
            {
                return missing("the Agility SDK runtime is required beside the executable, whose path could not be read");
            }
            const std::wstring directory = executable.substr(0, slash) + L"\\D3D12\\";
            const std::string path = narrow(directory);
            Com<ID3D12DeviceFactory> factory;
            if (const HRESULT created = configuration->CreateDeviceFactory(D3D12_SDK_VERSION, path.c_str(), IID_PPV_ARGS(factory.put())); FAILED(created))
            {
                return missing(
                    std::format(
                        "the Agility SDK runtime (D3D12Core.dll, SDK version {}) is required in {}: CreateDeviceFactory failed with HRESULT 0x{:08X}",
                        D3D12_SDK_VERSION,
                        utf8(directory),
                        hresultBits(created)
                    )
                );
            }
            return factory;
        }

        //==============================================================================================================
        /// Enables on `factory`, never process-wide, the debug features the build selects: the debug layer and DRED's
        /// breadcrumbs, breadcrumb contexts and page-fault reports, and GPU-based validation for `gbv`. A configuration
        /// interface the factory refuses fails creation, because the build asked for it.
        Result<void> configureDebug([[maybe_unused]] ID3D12DeviceFactory* factory)
        {
            if constexpr (kDebugLayer)
            {
                Com<ID3D12Debug1> debug;
                if (const HRESULT got = factory->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(debug.put())); FAILED(got))
                {
                    return missing(
                        std::format(
                            "the D3D12 debug layer the build selects is unavailable (d3d12SDKLayers.dll beside D3D12Core.dll): "
                            "GetConfigurationInterface(CLSID_D3D12Debug) "
                            "failed with HRESULT 0x{:08X}",
                            hresultBits(got)
                        )
                    );
                }
                debug->EnableDebugLayer();
                if constexpr (kGpuValidation)
                {
                    debug->SetEnableGPUBasedValidation(TRUE);
                }
                Com<ID3D12DeviceRemovedExtendedDataSettings1> dred;
                if (const HRESULT got = factory->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData, IID_PPV_ARGS(dred.put())); FAILED(got))
                {
                    return missing(
                        std::format(
                            "DRED, which the build selects with the debug layer, is unavailable: "
                            "GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData) failed "
                            "with HRESULT 0x{:08X}",
                            hresultBits(got)
                        )
                    );
                }
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            }
            return {};
        }

        //==============================================================================================================
        /// Returns the adapters of the configured kind, in the order devices are tried: WARP alone, or every hardware adapter
        /// by high-performance preference, software adapters skipped.
        Result<std::vector<Com<IDXGIAdapter1>>> candidateAdapters(IDXGIFactory6* dxgi)
        {
            std::vector<Com<IDXGIAdapter1>> candidates;
            if constexpr (kWarpAdapter)
            {
                Com<IDXGIAdapter1> warp;
                if (const HRESULT got = dxgi->EnumWarpAdapter(IID_PPV_ARGS(warp.put())); FAILED(got))
                {
                    return missing(
                        std::format("the WARP adapter the build selects is unavailable: EnumWarpAdapter failed with HRESULT 0x{:08X}", hresultBits(got))
                    );
                }
                candidates.push_back(std::move(warp));
            }
            else
            {
                for (UINT index = 0;; ++index)
                {
                    Com<IDXGIAdapter4> adapter;
                    const HRESULT got = dxgi->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.put()));
                    if (got == DXGI_ERROR_NOT_FOUND)
                    {
                        break;
                    }
                    if (FAILED(got))
                    {
                        return std::unexpected(hresultError(got, "IDXGIFactory6::EnumAdapterByGpuPreference"));
                    }
                    DXGI_ADAPTER_DESC3 desc = {};
                    if (const HRESULT described = adapter->GetDesc3(&desc); FAILED(described))
                    {
                        return std::unexpected(hresultError(described, "IDXGIAdapter4::GetDesc3"));
                    }
                    if ((static_cast<UINT>(desc.Flags) & static_cast<UINT>(DXGI_ADAPTER_FLAG3_SOFTWARE)) != 0)
                    {
                        continue;
                    }
                    // IDXGIAdapter4 derives from IDXGIAdapter1, so the reference passes over unchanged.
                    candidates.emplace_back(adapter.detach());
                }
            }
            return candidates;
        }

        //==============================================================================================================
        /// Returns the adapter's description, for messages.
        std::string adapterName(IDXGIAdapter1* adapter)
        {
            DXGI_ADAPTER_DESC1 desc = {};
            if (FAILED(adapter->GetDesc1(&desc)))
            {
                return "an adapter";
            }
            return utf8(desc.Description);
        }
    } // namespace

    //==================================================================================================================
    Result<Loaded> loadDevice()
    {
        if (Result<void> windows = checkWindows(); !windows)
        {
            return std::unexpected(std::move(windows.error()));
        }
        Loaded loaded;
        loaded.layer = kDebugLayer;
        loaded.gpuValidation = kGpuValidation;
        Result<Com<ID3D12DeviceFactory>> factory = createFactory();
        if (!factory)
        {
            return std::unexpected(std::move(factory.error()));
        }
        loaded.factory = std::move(*factory);
        if (Result<void> debug = configureDebug(loaded.factory.get()); !debug)
        {
            return std::unexpected(std::move(debug.error()));
        }
        // No DXGI debug flag: dxgidebug.dll is not redistributable.
        if (const HRESULT created = CreateDXGIFactory2(0, IID_PPV_ARGS(loaded.dxgi.put())); FAILED(created))
        {
            return std::unexpected(hresultError(created, "CreateDXGIFactory2"));
        }
        Result<std::vector<Com<IDXGIAdapter1>>> candidates = candidateAdapters(loaded.dxgi.get());
        if (!candidates)
        {
            return std::unexpected(std::move(candidates.error()));
        }
        // The first candidate's first missing item names the failure when none passes.
        std::string firstMissing;
        for (Com<IDXGIAdapter1>& adapter : *candidates)
        {
            Com<ID3D12Device12> device;
            if (const HRESULT created = loaded.factory->CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(device.put())); FAILED(created))
            {
                if (firstMissing.empty())
                {
                    firstMissing = std::format(
                        "no adapter at D3D_FEATURE_LEVEL_12_2: {} refused it with HRESULT 0x{:08X}", adapterName(adapter.get()), hresultBits(created)
                    );
                }
                continue;
            }
            if (Result<void> floor = checkFloor(device.get()); !floor)
            {
                if (firstMissing.empty())
                {
                    firstMissing = std::format("{}: {}", adapterName(adapter.get()), floor.error().message);
                }
                continue;
            }
            loaded.adapter = std::move(adapter);
            loaded.device = std::move(device);
            return loaded;
        }
        return missing(firstMissing.empty() ? std::string("no adapter at D3D_FEATURE_LEVEL_12_2") : std::move(firstMissing));
    }
} // namespace rojoRHI::d3d12
