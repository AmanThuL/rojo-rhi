//----------------------------------------------------------------------------------------------------------------------
/// @file Environment.cpp
/// @brief Implements the environment writer over the native arm's D3D12 adapter, the runtime DLLs and RtlGetVersion.
//----------------------------------------------------------------------------------------------------------------------
#include "Host/Environment.h"

#include "Common/JsonWriter.h"
#include "Common/RunOptions.h"
#include "Host/ExecutableDirectory.h"
#include "Host/ThermalState.h"
#include "NativeD3D12/NativeContext.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    namespace {
        //==============================================================================================================
        // The file version of a module, "a.b.c.d", or nothing when the file is absent or carries no version resource.
        std::optional<std::string> fileVersion(const std::filesystem::path& path)
        {
            DWORD handle = 0;
            const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
            if (size == 0)
            {
                return std::nullopt;
            }
            std::vector<std::byte> data(size);
            if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
            {
                return std::nullopt;
            }
            void* block = nullptr;
            UINT length = 0;
            if (!VerQueryValueW(data.data(), L"\\", &block, &length) || block == nullptr || length < sizeof(VS_FIXEDFILEINFO))
            {
                return std::nullopt;
            }
            const auto* info = static_cast<const VS_FIXEDFILEINFO*>(block);
            return std::format(
                "{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)
            );
        }

        //==============================================================================================================
        // The user-mode driver version DXGI reports for the adapter, "a.b.c.d", or nothing when it reports none.
        std::optional<std::string> driverVersion(IDXGIAdapter1* adapter)
        {
            LARGE_INTEGER version{};
            if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
            {
                return std::nullopt;
            }
            const auto high = static_cast<uint32_t>(version.HighPart);
            const auto low = static_cast<uint32_t>(version.LowPart);
            return std::format("{}.{}.{}.{}", high >> 16, high & 0xFFFF, low >> 16, low & 0xFFFF);
        }

        //==============================================================================================================
        // The highest feature level of the device, as "12_2".
        std::optional<std::string> featureLevel(ID3D12Device12* device)
        {
            const D3D_FEATURE_LEVEL levels[] = {
                D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
            };
            D3D12_FEATURE_DATA_FEATURE_LEVELS data{};
            data.NumFeatureLevels = static_cast<UINT>(std::size(levels));
            data.pFeatureLevelsRequested = levels;
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &data, static_cast<UINT>(sizeof(data)))))
            {
                return std::nullopt;
            }
            const auto value = static_cast<uint32_t>(data.MaxSupportedFeatureLevel);
            return std::format("{}_{}", value >> 12, (value >> 8) & 0xF);
        }

        //==============================================================================================================
        // The highest shader model of the device, as "6.8". The runtime refuses a model newer than it knows with
        // E_INVALIDARG, so the request steps down until it is answered.
        std::optional<std::string> shaderModel(ID3D12Device12* device)
        {
            D3D12_FEATURE_DATA_SHADER_MODEL model{ D3D_HIGHEST_SHADER_MODEL };
            HRESULT result = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, static_cast<UINT>(sizeof(model)));
            while (result == E_INVALIDARG && model.HighestShaderModel > D3D_SHADER_MODEL_5_1)
            {
                model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(static_cast<int>(model.HighestShaderModel) - 1);
                result = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, static_cast<UINT>(sizeof(model)));
            }
            if (FAILED(result))
            {
                return std::nullopt;
            }
            const auto value = static_cast<uint32_t>(model.HighestShaderModel);
            return std::format("{}.{}", value >> 4, value & 0xF);
        }

        //==============================================================================================================
        // A string value, or null.
        void stringOrNull(JsonWriter& json, std::string_view key, const std::optional<std::string>& value)
        {
            json.key(key);
            if (value)
            {
                json.string(*value);
            }
            else
            {
                json.null();
            }
        }
    } // namespace

    //==================================================================================================================
    // The device keys come from a native context opened as the native arm opens one, so they describe the adapter both
    // arms run on; the runtime versions are those of the DLLs the build copies beside the executable.
    std::expected<void, std::string> writeEnvironmentFile(const std::filesystem::path& path)
    {
        NativeContext context;
        const std::expected<void, std::string> opened = context.open(RunOptions{});
        const bool hasDevice = opened.has_value();
        const std::expected<std::filesystem::path, std::string> directory = executableDirectory();

        JsonWriter json;
        json.beginObject();
        if (hasDevice)
        {
            const DXGI_ADAPTER_DESC1& desc = context.adapterDesc();
            json.key("device").string(context.adapterName());
            json.key("adapter").string(
                std::format("{:08x}:{:08x}", static_cast<uint32_t>(desc.AdapterLuid.HighPart), static_cast<uint32_t>(desc.AdapterLuid.LowPart))
            );
            json.key("vendorId").unsignedInteger(desc.VendorId);
            json.key("deviceId").unsignedInteger(desc.DeviceId);
            stringOrNull(json, "driverVersion", driverVersion(context.adapter()));
            stringOrNull(json, "featureLevel", featureLevel(context.device()));
            stringOrNull(json, "shaderModel", shaderModel(context.device()));
        }
        else
        {
            for (const std::string_view key : { "device", "adapter", "vendorId", "deviceId", "driverVersion", "featureLevel", "shaderModel" })
            {
                json.key(key).null();
            }
        }
        stringOrNull(json, "agility", directory ? fileVersion(*directory / "D3D12" / "D3D12Core.dll") : std::nullopt);
        stringOrNull(json, "warp", directory ? fileVersion(*directory / "d3d10warp.dll") : std::nullopt);
        json.key("timestampHz");
        uint64_t frequency = 0;
        if (hasDevice && SUCCEEDED(context.graphicsQueue()->GetTimestampFrequency(&frequency)))
        {
            json.unsignedInteger(frequency);
        }
        else
        {
            json.null();
        }
        json.key("os").string(osVersion());
        json.key("thermal").string(thermalState());
        json.endObject();

        auto saved = json.save(path);
        if (!saved)
        {
            return saved;
        }
        if (!hasDevice)
        {
            return std::unexpected("environment: the native arm opened no device: " + opened.error());
        }
        return {};
    }

    //==================================================================================================================
    std::string deviceName()
    {
        NativeContext context;
        if (!context.open(RunOptions{}))
        {
            return "none";
        }
        return context.adapterName();
    }

    //==================================================================================================================
    // RtlGetVersion reports the true version whatever the executable's compatibility manifest says, unlike GetVersionEx.
    std::string osVersion()
    {
        using RtlGetVersionFunction = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto rtlGetVersion = ntdll != nullptr ? reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        RTL_OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = static_cast<ULONG>(sizeof(version));
        if (rtlGetVersion == nullptr || rtlGetVersion(&version) != 0)
        {
            return "Windows";
        }
        return std::format("Windows {}.{}.{}", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    }
} // namespace rojoRHI::bench
