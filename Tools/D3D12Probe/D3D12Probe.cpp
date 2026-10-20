//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Probe.cpp
/// @brief Prints, as one JSON object, whether the WARP adapter meets the D3D12 hardware floor and how the runtime loaded.
//----------------------------------------------------------------------------------------------------------------------

#include <windows.h>

#include <initguid.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <psapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

constexpr UINT kSdkVersion = 619;
static_assert(D3D12_SDK_VERSION == kSdkVersion, "the Agility SDK headers are not version 619");
constexpr size_t kMessagesPerPhase = 64;
constexpr UINT64 kRawViewElements = UINT64{ 1 } << 27;
constexpr UINT kWindowSize = 256;
constexpr const wchar_t* kWindowClass = L"RojoRHID3D12Probe";
constexpr const char* kRelativeSdkPath = ".\\D3D12\\";

#if defined(ROJORHI_D3D12_PROBE_EXPORTS)
constexpr bool kExportsVariant = true;
#else
constexpr bool kExportsVariant = false;
#endif
constexpr const char* kVariant = kExportsVariant ? "exports" : "factory";

// An owned COM interface. The move constructor makes it non-copyable, so each reference is released once.
template<typename T> struct Com
{
    //==================================================================================================================
    Com() : p(nullptr)
    {
    }

    //==================================================================================================================
    Com(Com&& other) noexcept : p(other.p)
    {
        other.p = nullptr;
    }

    //==================================================================================================================
    ~Com()
    {
        if (p != nullptr)
        {
            p->Release();
        }
    }

    T* p;
};

struct Message
{
    std::string phase;
    std::string severity;
    int id;
    std::string text;
};

// Debug-layer messages by the phase that raised them. The runtime may call back on its own threads, so the log is locked.
struct MessageLog
{
    std::mutex mutex;
    std::string phase = "start";
    std::vector<Message> messages;
    std::map<std::string, size_t> counts;
    std::map<std::string, size_t> errors;
};

struct Attempt
{
    std::string what;
    HRESULT result;
};

struct LoadedModule
{
    std::wstring path;
    std::string fileVersion;
    bool appLocal;
};

// Everything the probe measured. A call result left empty was never made.
struct Probe
{
    // Process
    std::optional<RTL_OSVERSIONINFOW> os;
    std::optional<DWORD> ubr;
    std::wstring exeDir;

    // Agility SDK
    std::string agilityMethod;
    std::string sdkPath;
    std::optional<HRESULT> sdkConfigResult;
    std::optional<HRESULT> agilityResult;
    std::vector<Attempt> factoryAttempts;
    std::vector<LoadedModule> agilityModules;

    // Debug configuration and status
    std::optional<HRESULT> debugResult;
    std::optional<HRESULT> dredResult;
    bool layerRequested = false;
    bool gbvRequested = false;
    bool gbvActive = false;
    bool dredConfigured = false;
    bool layerActive = false;
    bool dredActive = false;
    std::optional<HRESULT> infoQueue1Result;
    bool infoQueue1Callback = false;

    // Adapter and device
    std::optional<HRESULT> dxgiFactoryResult;
    std::optional<HRESULT> adapterResult;
    std::optional<DXGI_ADAPTER_DESC1> adapterDesc;
    std::vector<Attempt> deviceAttempts;
    std::optional<D3D_FEATURE_LEVEL> deviceFeatureLevel;
    bool device12 = false;
    std::vector<LoadedModule> warpModules;

    // Features
    std::vector<Attempt> queries;
    std::optional<D3D_FEATURE_LEVEL> maxFeatureLevel;
    std::optional<D3D_SHADER_MODEL> highestShaderModel;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS19 options19 = {};
    bool options19Reported = false;
    std::vector<Attempt> viewHeapAttempts;
    std::optional<UINT> largestViewHeap;

    // Raw buffer views of 2^27 and 2^27 + 1 elements
    std::optional<HRESULT> rawBufferResult;
    std::optional<HRESULT> rawHeapResult;
    std::optional<bool> rawViewErrors[2];

    // Presentation
    std::optional<HRESULT> windowResult;
    std::optional<HRESULT> queueResult;
    std::optional<HRESULT> swapchainResult;
    std::vector<std::string> waits;
    std::vector<UINT> backBufferIndices;
    std::vector<HRESULT> presentResults;

    // Objects, released in reverse order
    MessageLog log;
    Com<ID3D12SDKConfiguration1> sdkConfig;
    Com<ID3D12DeviceFactory> deviceFactory;
    Com<ID3D12Debug3> debugConfig;
    Com<ID3D12DeviceRemovedExtendedDataSettings1> dredConfig;
    Com<IDXGIFactory4> dxgiFactory;
    Com<IDXGIAdapter1> adapter;
    Com<ID3D12Device> device;
    Com<ID3D12InfoQueue> infoQueue;
    Com<ID3D12InfoQueue1> infoQueue1;
    DWORD callbackCookie = 0;
};

// A JSON text written front to back; each open object or array records whether it holds a member yet.
struct Json
{
    std::string text;
    std::vector<bool> written;
};

//======================================================================================================================
template<typename T> void release(Com<T>& owner)
{
    if (owner.p != nullptr)
    {
        owner.p->Release();
        owner.p = nullptr;
    }
}

//======================================================================================================================
template<typename T, typename U> HRESULT queryInterface(U* object, Com<T>& out)
{
    release(out);
    return object->QueryInterface(IID_PPV_ARGS(&out.p));
}

//======================================================================================================================
static std::string narrow(std::wstring_view text, UINT codePage)
{
    if (text.empty())
    {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(codePage, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(codePage, 0, text.data(), length, out.data(), size, nullptr, nullptr);
    return out;
}

//======================================================================================================================
static std::string utf8(std::wstring_view text)
{
    return narrow(text, CP_UTF8);
}

//======================================================================================================================
// Debug-layer descriptions arrive in the ANSI code page; the JSON is UTF-8.
static std::string fromAnsi(const char* text)
{
    if (text == nullptr || *text == '\0')
    {
        return {};
    }
    const int size = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (size <= 1)
    {
        return {};
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), size);
    wide.resize(static_cast<size_t>(size - 1));
    return utf8(wide);
}

//======================================================================================================================
static bool equalsIgnoringCase(std::wstring_view a, std::wstring_view b)
{
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

//======================================================================================================================
static std::wstring_view directoryOf(std::wstring_view path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? std::wstring_view() : path.substr(0, slash);
}

//======================================================================================================================
static std::wstring modulePath(HMODULE loaded)
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(loaded, path.data(), static_cast<DWORD>(path.size()));
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

//======================================================================================================================
static std::string fileVersion(const std::wstring& path)
{
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0)
    {
        return {};
    }
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
    {
        return {};
    }
    void* block = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", &block, &length) || block == nullptr || length < sizeof(VS_FIXEDFILEINFO))
    {
        return {};
    }
    const auto* info = static_cast<const VS_FIXEDFILEINFO*>(block);
    return std::format(
        "{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)
    );
}

//======================================================================================================================
// Every loaded module of that file name, wherever it loaded from: two copies of one DLL can be loaded at once.
static std::vector<LoadedModule> loadedModules(std::wstring_view name, std::wstring_view appLocalDir)
{
    std::vector<HMODULE> modules(512);
    DWORD needed = 0;
    for (;;)
    {
        const DWORD bytes = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
        if (!EnumProcessModules(GetCurrentProcess(), modules.data(), bytes, &needed))
        {
            return {};
        }
        if (needed <= bytes)
        {
            break;
        }
        modules.resize(needed / sizeof(HMODULE));
    }
    modules.resize(needed / sizeof(HMODULE));
    std::vector<LoadedModule> found;
    for (HMODULE loaded : modules)
    {
        const std::wstring path = modulePath(loaded);
        const std::wstring_view directory = directoryOf(path);
        const std::wstring_view base = directory.empty() ? std::wstring_view(path) : std::wstring_view(path).substr(directory.size() + 1);
        if (equalsIgnoringCase(base, name))
        {
            found.push_back({ path, fileVersion(path), equalsIgnoringCase(directory, appLocalDir) });
        }
    }
    return found;
}

//======================================================================================================================
static const char* severityName(D3D12_MESSAGE_SEVERITY severity)
{
    switch (severity)
    {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "corruption";
    case D3D12_MESSAGE_SEVERITY_ERROR: return "error";
    case D3D12_MESSAGE_SEVERITY_WARNING: return "warning";
    case D3D12_MESSAGE_SEVERITY_INFO: return "info";
    case D3D12_MESSAGE_SEVERITY_MESSAGE: return "message";
    }
    return "unknown";
}

//======================================================================================================================
static void recordMessage(MessageLog& messageLog, D3D12_MESSAGE_SEVERITY severity, int id, std::string text)
{
    std::lock_guard lock(messageLog.mutex);
    size_t& count = messageLog.counts[messageLog.phase];
    ++count;
    if (severity == D3D12_MESSAGE_SEVERITY_ERROR || severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
    {
        ++messageLog.errors[messageLog.phase];
    }
    if (count <= kMessagesPerPhase)
    {
        messageLog.messages.push_back({ messageLog.phase, severityName(severity), id, std::move(text) });
    }
}

//======================================================================================================================
// Files each debug-layer message under the current phase and never stops the probe.
static void __stdcall onMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void* context)
{
    recordMessage(*static_cast<MessageLog*>(context), severity, static_cast<int>(id), fromAnsi(description));
}

//======================================================================================================================
// Without a message callback, the messages the info queue stored are read at each phase boundary instead.
static void drainStoredMessages(Probe& probe)
{
    ID3D12InfoQueue* queue = probe.infoQueue.p;
    if (queue == nullptr || probe.infoQueue1Callback)
    {
        return;
    }
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; ++index)
    {
        SIZE_T size = 0;
        if (FAILED(queue->GetMessage(index, nullptr, &size)) || size == 0)
        {
            continue;
        }
        std::vector<std::byte> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(queue->GetMessage(index, message, &size)))
        {
            recordMessage(probe.log, message->Severity, static_cast<int>(message->ID), fromAnsi(message->pDescription));
        }
    }
    queue->ClearStoredMessages();
}

//======================================================================================================================
// Messages the debug layer raises from here on are filed under this phase.
static void beginPhase(Probe& probe, const std::string& phase)
{
    drainStoredMessages(probe);
    {
        std::lock_guard lock(probe.log.mutex);
        probe.log.phase = phase;
    }
    std::fprintf(stderr, "d3d12 probe (%s): %s\n", kVariant, phase.c_str());
}

//======================================================================================================================
// Whether the phase raised an error or corruption message; unknown when no message reaches the probe.
static std::optional<bool> phaseRaisedError(Probe& probe, const std::string& phase)
{
    drainStoredMessages(probe);
    if (!probe.infoQueue1Callback && probe.infoQueue.p == nullptr)
    {
        return std::nullopt;
    }
    std::lock_guard lock(probe.log.mutex);
    const auto found = probe.log.errors.find(phase);
    return found != probe.log.errors.end() && found->second > 0;
}

//======================================================================================================================
static std::string featureLevelName(D3D_FEATURE_LEVEL level)
{
    const int value = static_cast<int>(level);
    return std::format("{}_{}", (value >> 12) & 0xF, (value >> 8) & 0xF);
}

//======================================================================================================================
static std::string shaderModelName(D3D_SHADER_MODEL model)
{
    const int value = static_cast<int>(model);
    return std::format("{}.{}", value >> 4, value & 0xF);
}

//======================================================================================================================
static D3D_SHADER_MODEL previousShaderModel(D3D_SHADER_MODEL model)
{
    const int value = static_cast<int>(model);
    return static_cast<D3D_SHADER_MODEL>((value & 0xF) > 0 ? value - 1 : static_cast<int>(D3D_SHADER_MODEL_5_1));
}

//======================================================================================================================
static void probeProcess(Probe& probe)
{
    using RtlGetVersionFunction = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtlGetVersion = ntdll != nullptr ? reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    RTL_OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = static_cast<ULONG>(sizeof(version));
    if (rtlGetVersion != nullptr && rtlGetVersion(&version) == 0)
    {
        probe.os = version;
    }
    DWORD ubr = 0;
    DWORD size = static_cast<DWORD>(sizeof(ubr));
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size) == ERROR_SUCCESS)
    {
        probe.ubr = ubr;
    }
    probe.exeDir = std::wstring(directoryOf(modulePath(nullptr)));
}

//======================================================================================================================
// The factory variant asks the in-box runtime for a device factory on the Agility SDK beside the executable, first by
// absolute path, then by the relative one the exports use. The exports variant has loaded it before main.
static void loadAgility(Probe& probe)
{
    beginPhase(probe, "agility");
    if constexpr (kExportsVariant)
    {
        probe.agilityMethod = "exports";
        probe.sdkPath = kRelativeSdkPath;
    }
    else
    {
        probe.agilityMethod = "CreateDeviceFactory";
        probe.sdkConfigResult = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&probe.sdkConfig.p));
        probe.agilityResult = probe.sdkConfigResult;
        if (FAILED(*probe.sdkConfigResult))
        {
            return;
        }
        const std::string absolutePath = narrow(probe.exeDir + L"\\D3D12\\", CP_ACP);
        for (const std::string& sdkPath : { absolutePath, std::string(kRelativeSdkPath) })
        {
            release(probe.deviceFactory);
            const HRESULT result = probe.sdkConfig.p->CreateDeviceFactory(kSdkVersion, sdkPath.c_str(), IID_PPV_ARGS(&probe.deviceFactory.p));
            probe.factoryAttempts.push_back({ sdkPath, result });
            probe.agilityResult = result;
            if (SUCCEEDED(result))
            {
                probe.sdkPath = sdkPath;
                return;
            }
        }
    }
}

//======================================================================================================================
// The debug layer with GPU-based validation and DRED, configured before the device exists: on the device factory in the
// factory variant, process-wide in the exports variant.
static void configureDebug(Probe& probe)
{
    beginPhase(probe, "debug");
    if constexpr (kExportsVariant)
    {
        probe.debugResult = D3D12GetDebugInterface(IID_PPV_ARGS(&probe.debugConfig.p));
        probe.dredResult = D3D12GetDebugInterface(IID_PPV_ARGS(&probe.dredConfig.p));
    }
    else
    {
        if (probe.deviceFactory.p == nullptr)
        {
            return;
        }
        probe.debugResult = probe.deviceFactory.p->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&probe.debugConfig.p));
        probe.dredResult = probe.deviceFactory.p->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData, IID_PPV_ARGS(&probe.dredConfig.p));
    }
    if (SUCCEEDED(*probe.debugResult))
    {
        probe.debugConfig.p->EnableDebugLayer();
        probe.debugConfig.p->SetEnableGPUBasedValidation(TRUE);
        probe.layerRequested = true;
        probe.gbvRequested = true;
    }
    if (SUCCEEDED(*probe.dredResult))
    {
        probe.dredConfig.p->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        probe.dredConfig.p->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        probe.dredConfigured = true;
    }
}

//======================================================================================================================
// A device on the WARP adapter at 12_2; when that fails, at 11_0, so the features are still measured.
static void createDevice(Probe& probe)
{
    beginPhase(probe, "adapter");
    probe.dxgiFactoryResult = CreateDXGIFactory2(0, IID_PPV_ARGS(&probe.dxgiFactory.p));
    if (FAILED(*probe.dxgiFactoryResult))
    {
        return;
    }
    probe.adapterResult = probe.dxgiFactory.p->EnumWarpAdapter(IID_PPV_ARGS(&probe.adapter.p));
    if (FAILED(*probe.adapterResult))
    {
        return;
    }
    DXGI_ADAPTER_DESC1 desc = {};
    if (SUCCEEDED(probe.adapter.p->GetDesc1(&desc)))
    {
        probe.adapterDesc = desc;
    }

    beginPhase(probe, "device");
    if constexpr (!kExportsVariant)
    {
        if (probe.deviceFactory.p == nullptr)
        {
            return;
        }
    }
    for (const D3D_FEATURE_LEVEL level : { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_11_0 })
    {
        HRESULT result = S_OK;
        if constexpr (kExportsVariant)
        {
            result = D3D12CreateDevice(probe.adapter.p, level, IID_PPV_ARGS(&probe.device.p));
        }
        else
        {
            result = probe.deviceFactory.p->CreateDevice(probe.adapter.p, level, IID_PPV_ARGS(&probe.device.p));
        }
        probe.deviceAttempts.push_back({ featureLevelName(level), result });
        if (SUCCEEDED(result))
        {
            probe.deviceFeatureLevel = level;
            break;
        }
    }
    if (probe.device.p != nullptr)
    {
        Com<ID3D12Device12> device12;
        probe.device12 = SUCCEEDED(queryInterface(probe.device.p, device12));
    }
}

//======================================================================================================================
static void findRuntimeModules(Probe& probe)
{
    probe.agilityModules = loadedModules(L"D3D12Core.dll", probe.exeDir + L"\\D3D12");
    probe.warpModules = loadedModules(L"d3d10warp.dll", probe.exeDir);
}

//======================================================================================================================
// What of the debug configuration reached the device, and the message callback every later phase reports through.
static void probeDebugStatus(Probe& probe)
{
    Com<ID3D12DebugDevice> debugDevice;
    probe.layerActive = SUCCEEDED(queryInterface(probe.device.p, debugDevice));
    Com<ID3D12DeviceRemovedExtendedData> dred;
    probe.dredActive = SUCCEEDED(queryInterface(probe.device.p, dred));
    if (SUCCEEDED(queryInterface(probe.device.p, probe.infoQueue)))
    {
        // Messages raised while the device was created, filed under the device phase.
        drainStoredMessages(probe);
    }
    // No query reports GPU-based validation; the debug layer announces it in its startup message at device creation.
    {
        std::lock_guard lock(probe.log.mutex);
        probe.gbvActive = std::any_of(probe.log.messages.begin(), probe.log.messages.end(), [](const Message& message) {
            return message.phase == "device" && message.id == static_cast<int>(D3D12_MESSAGE_ID_CREATEDEVICE_DEBUG_LAYER_STARTUP_OPTIONS) &&
                   message.text.find("GPU-Based Validation is enabled") != std::string::npos;
        });
    }
    probe.infoQueue1Result = queryInterface(probe.device.p, probe.infoQueue1);
    if (SUCCEEDED(*probe.infoQueue1Result))
    {
        probe.infoQueue1Result =
            probe.infoQueue1.p->RegisterMessageCallback(onMessage, D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, &probe.log, &probe.callbackCookie);
        probe.infoQueue1Callback = SUCCEEDED(*probe.infoQueue1Result);
    }
}

//======================================================================================================================
template<typename T> static HRESULT checkFeature(Probe& probe, const char* name, D3D12_FEATURE feature, T& data)
{
    const HRESULT result = probe.device.p->CheckFeatureSupport(feature, &data, static_cast<UINT>(sizeof(T)));
    probe.queries.push_back({ name, result });
    return result;
}

//======================================================================================================================
static void probeFeatures(Probe& probe)
{
    beginPhase(probe, "features");
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};
    featureLevels.NumFeatureLevels = static_cast<UINT>(std::size(levels));
    featureLevels.pFeatureLevelsRequested = levels;
    if (SUCCEEDED(checkFeature(probe, "FEATURE_LEVELS", D3D12_FEATURE_FEATURE_LEVELS, featureLevels)))
    {
        probe.maxFeatureLevel = featureLevels.MaxSupportedFeatureLevel;
    }

    // A runtime answers E_INVALIDARG for a shader model newer than it knows, so the query steps down from the newest.
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_HIGHEST_SHADER_MODEL };
    HRESULT result = checkFeature(probe, "SHADER_MODEL", D3D12_FEATURE_SHADER_MODEL, shaderModel);
    while (result == E_INVALIDARG && shaderModel.HighestShaderModel > D3D_SHADER_MODEL_5_1)
    {
        shaderModel.HighestShaderModel = previousShaderModel(shaderModel.HighestShaderModel);
        result = checkFeature(probe, "SHADER_MODEL", D3D12_FEATURE_SHADER_MODEL, shaderModel);
    }
    if (SUCCEEDED(result))
    {
        probe.highestShaderModel = shaderModel.HighestShaderModel;
    }

    checkFeature(probe, "OPTIONS", D3D12_FEATURE_D3D12_OPTIONS, probe.options);
    checkFeature(probe, "OPTIONS3", D3D12_FEATURE_D3D12_OPTIONS3, probe.options3);
    checkFeature(probe, "OPTIONS12", D3D12_FEATURE_D3D12_OPTIONS12, probe.options12);
    checkFeature(probe, "OPTIONS16", D3D12_FEATURE_D3D12_OPTIONS16, probe.options16);
    probe.options19Reported = SUCCEEDED(checkFeature(probe, "OPTIONS19", D3D12_FEATURE_D3D12_OPTIONS19, probe.options19));
}

//======================================================================================================================
// The largest shader-visible view heap of the candidates, tried largest first.
static void probeViewHeap(Probe& probe)
{
    std::vector<UINT> candidates = { 1u << 21, 1000000u };
    if (probe.options19Reported && probe.options19.MaxViewDescriptorHeapSize > 0)
    {
        candidates.push_back(probe.options19.MaxViewDescriptorHeapSize);
    }
    std::sort(candidates.begin(), candidates.end(), [](UINT a, UINT b) { return a > b; });
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    for (const UINT size : candidates)
    {
        beginPhase(probe, std::format("viewHeap{}", size));
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = size;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Com<ID3D12DescriptorHeap> heap;
        const HRESULT result = probe.device.p->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap.p));
        probe.viewHeapAttempts.push_back({ std::to_string(size), result });
        if (SUCCEEDED(result))
        {
            probe.largestViewHeap = size;
            return;
        }
    }
}

//======================================================================================================================
// Raw SRVs of 2^27 and 2^27 + 1 elements over one buffer large enough for both, each in its own message phase.
static void probeRawViews(Probe& probe)
{
    beginPhase(probe, "rawViewSetup");
    D3D12_HEAP_PROPERTIES heapProperties = {};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (kRawViewElements + 1) * 4;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Com<ID3D12Resource> buffer;
    probe.rawBufferResult =
        probe.device.p->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer.p));
    if (FAILED(*probe.rawBufferResult))
    {
        return;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    Com<ID3D12DescriptorHeap> heap;
    probe.rawHeapResult = probe.device.p->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap.p));
    if (FAILED(*probe.rawHeapResult))
    {
        return;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE start = heap.p->GetCPUDescriptorHandleForHeapStart();
    const UINT increment = probe.device.p->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const char* const phases[] = { "rawView2p27", "rawView2p27plus1" };
    for (UINT index = 0; index < 2; ++index)
    {
        beginPhase(probe, phases[index]);
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.FirstElement = 0;
        view.Buffer.NumElements = static_cast<UINT>(kRawViewElements + index);
        view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = start;
        handle.ptr += static_cast<SIZE_T>(index) * increment;
        probe.device.p->CreateShaderResourceView(buffer.p, &view, handle);
        probe.rawViewErrors[index] = phaseRaisedError(probe, phases[index]);
    }
    beginPhase(probe, "rawViewTeardown");
}

//======================================================================================================================
static const char* waitName(DWORD wait)
{
    switch (wait)
    {
    case WAIT_OBJECT_0: return "signaled";
    case WAIT_TIMEOUT: return "timeout";
    case WAIT_ABANDONED: return "abandoned";
    case WAIT_FAILED: return "failed";
    }
    return "unknown";
}

//======================================================================================================================
// A flip-model swapchain on the hidden window: two frames, each waited for, then presented.
static void presentOnWindow(Probe& probe, HWND window)
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Com<ID3D12CommandQueue> queue;
    probe.queueResult = probe.device.p->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue.p));
    if (FAILED(*probe.queueResult))
    {
        return;
    }
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = kWindowSize;
    desc.Height = kWindowSize;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    Com<IDXGISwapChain1> swapchain1;
    probe.swapchainResult = probe.dxgiFactory.p->CreateSwapChainForHwnd(queue.p, window, &desc, nullptr, nullptr, &swapchain1.p);
    if (FAILED(*probe.swapchainResult))
    {
        return;
    }
    Com<IDXGISwapChain3> swapchain;
    probe.swapchainResult = queryInterface(swapchain1.p, swapchain);
    if (FAILED(*probe.swapchainResult))
    {
        return;
    }
    const HANDLE waitable = swapchain.p->GetFrameLatencyWaitableObject();
    for (int frame = 0; frame < 2; ++frame)
    {
        probe.waits.push_back(waitable != nullptr ? waitName(WaitForSingleObject(waitable, 1000)) : "noWaitableObject");
        probe.backBufferIndices.push_back(swapchain.p->GetCurrentBackBufferIndex());
        probe.presentResults.push_back(swapchain.p->Present(1, 0));
    }
    if (waitable != nullptr)
    {
        CloseHandle(waitable);
    }
    // The presents are queue work; the queue drains before the swapchain and window go away.
    Com<ID3D12Fence> fence;
    if (SUCCEEDED(probe.device.p->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))) && SUCCEEDED(queue.p->Signal(fence.p, 1)))
    {
        fence.p->SetEventOnCompletion(1, nullptr);
    }
}

//======================================================================================================================
static void probePresentation(Probe& probe)
{
    beginPhase(probe, "present");
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = static_cast<UINT>(sizeof(windowClass));
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassExW(&windowClass) == 0)
    {
        probe.windowResult = HRESULT_FROM_WIN32(GetLastError());
        return;
    }
    const int size = static_cast<int>(kWindowSize);
    const HWND window = CreateWindowExW(
        0, kWindowClass, L"RojoRHI D3D12 probe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, size, size, nullptr, nullptr, instance, nullptr
    );
    probe.windowResult = window != nullptr ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    if (window != nullptr)
    {
        presentOnWindow(probe, window);
        DestroyWindow(window);
    }
    UnregisterClassW(kWindowClass, instance);
}

//======================================================================================================================
// The floor items of ADR 0010 and ResourceHeapTier 2 that the device misses, by their JSON names.
static std::vector<std::string> missingFloorItems(const Probe& probe)
{
    if (probe.device.p == nullptr)
    {
        return { "device" };
    }
    const bool featureLevel =
        probe.deviceFeatureLevel == D3D_FEATURE_LEVEL_12_2 && probe.maxFeatureLevel.value_or(D3D_FEATURE_LEVEL_11_0) >= D3D_FEATURE_LEVEL_12_2;
    const std::pair<const char*, bool> items[] = {
        { "maxFeatureLevel", featureLevel },
        { "highestShaderModel", probe.highestShaderModel.value_or(D3D_SHADER_MODEL_5_1) >= D3D_SHADER_MODEL_6_6 },
        { "resourceBindingTier", probe.options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3 },
        { "typedUavLoadAdditionalFormats", probe.options.TypedUAVLoadAdditionalFormats != FALSE },
        { "enhancedBarriersSupported", probe.options12.EnhancedBarriersSupported != FALSE },
        { "dynamicDepthBiasSupported", probe.options16.DynamicDepthBiasSupported != FALSE },
        { "resourceHeapTier", probe.options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2 },
    };
    std::vector<std::string> missing;
    for (const auto& [name, met] : items)
    {
        if (!met)
        {
            missing.push_back(name);
        }
    }
    return missing;
}

//======================================================================================================================
static std::string quote(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        const auto code = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (code < 0x20)
        {
            out += std::format("\\u{:04x}", static_cast<unsigned>(code));
        }
        else
        {
            out += c;
        }
    }
    out += '"';
    return out;
}

//======================================================================================================================
static std::string jsonBool(bool value)
{
    return value ? "true" : "false";
}

//======================================================================================================================
static std::string hresult(HRESULT result)
{
    return std::format("\"0x{:08X}\"", static_cast<uint32_t>(result));
}

//======================================================================================================================
static std::string hresult(const std::optional<HRESULT>& result)
{
    return result ? hresult(*result) : "null";
}

//======================================================================================================================
static void separate(Json& json, std::string_view key)
{
    if (!json.written.empty())
    {
        if (json.written.back())
        {
            json.text += ',';
        }
        json.written.back() = true;
    }
    if (!key.empty())
    {
        json.text += quote(key);
        json.text += ':';
    }
}

//======================================================================================================================
static void open(Json& json, std::string_view key, char bracket)
{
    separate(json, key);
    json.text += bracket;
    json.written.push_back(false);
}

//======================================================================================================================
static void close(Json& json, char bracket)
{
    json.text += bracket;
    json.written.pop_back();
}

//======================================================================================================================
// Writes a member (or, with an empty key, an array element) whose value is already JSON.
static void member(Json& json, std::string_view key, std::string_view value)
{
    separate(json, key);
    json.text += value;
}

//======================================================================================================================
static void writeAttempts(Json& json, std::string_view key, std::string_view whatKey, const std::vector<Attempt>& attempts)
{
    open(json, key, '[');
    for (const Attempt& attempt : attempts)
    {
        open(json, "", '{');
        member(json, whatKey, quote(attempt.what));
        member(json, "hresult", hresult(attempt.result));
        close(json, '}');
    }
    close(json, ']');
}

//======================================================================================================================
// The module the runtime uses: the app-local copy when one loaded, else the first copy found.
static const LoadedModule* chosenModule(const std::vector<LoadedModule>& modules)
{
    for (const LoadedModule& loaded : modules)
    {
        if (loaded.appLocal)
        {
            return &loaded;
        }
    }
    return modules.empty() ? nullptr : &modules.front();
}

//======================================================================================================================
// path, fileVersion and appLocal of the chosen module, then every copy that loaded. Every copy must be app-local, so a
// system copy loaded beside the app-local one makes appLocal false.
static void writeModules(Json& json, const std::vector<LoadedModule>& modules)
{
    const LoadedModule* chosen = chosenModule(modules);
    const bool appLocal = !modules.empty() && std::all_of(modules.begin(), modules.end(), [](const LoadedModule& loaded) { return loaded.appLocal; });
    member(json, "path", chosen != nullptr ? quote(utf8(chosen->path)) : "null");
    member(json, "fileVersion", chosen != nullptr ? quote(chosen->fileVersion) : "null");
    member(json, "appLocal", jsonBool(appLocal));
    open(json, "loaded", '[');
    for (const LoadedModule& loaded : modules)
    {
        open(json, "", '{');
        member(json, "path", quote(utf8(loaded.path)));
        member(json, "fileVersion", quote(loaded.fileVersion));
        member(json, "appLocal", jsonBool(loaded.appLocal));
        close(json, '}');
    }
    close(json, ']');
}

//======================================================================================================================
static bool warpAppLocal(const Probe& probe)
{
    return !probe.warpModules.empty() &&
           std::all_of(probe.warpModules.begin(), probe.warpModules.end(), [](const LoadedModule& loaded) { return loaded.appLocal; });
}

//======================================================================================================================
static std::string jsonOptionalBool(const std::optional<bool>& value)
{
    return value ? jsonBool(*value) : "null";
}

//======================================================================================================================
static void writeFeatures(Json& json, const Probe& probe)
{
    open(json, "features", '{');
    member(json, "maxFeatureLevel", probe.maxFeatureLevel ? quote(featureLevelName(*probe.maxFeatureLevel)) : "null");
    member(json, "highestShaderModel", probe.highestShaderModel ? quote(shaderModelName(*probe.highestShaderModel)) : "null");
    member(json, "resourceBindingTier", std::to_string(static_cast<int>(probe.options.ResourceBindingTier)));
    member(json, "typedUavLoadAdditionalFormats", jsonBool(probe.options.TypedUAVLoadAdditionalFormats != FALSE));
    member(json, "resourceHeapTier", std::to_string(static_cast<int>(probe.options.ResourceHeapTier)));
    member(json, "tiledResourcesTier", std::to_string(static_cast<int>(probe.options.TiledResourcesTier)));
    member(json, "conservativeRasterizationTier", std::to_string(static_cast<int>(probe.options.ConservativeRasterizationTier)));
    member(json, "enhancedBarriersSupported", jsonBool(probe.options12.EnhancedBarriersSupported != FALSE));
    member(json, "relaxedFormatCastingSupported", jsonBool(probe.options12.RelaxedFormatCastingSupported != FALSE));
    member(json, "dynamicDepthBiasSupported", jsonBool(probe.options16.DynamicDepthBiasSupported != FALSE));
    member(json, "gpuUploadHeapSupported", jsonBool(probe.options16.GPUUploadHeapSupported != FALSE));
    member(json, "copyQueueTimestampQueriesSupported", jsonBool(probe.options3.CopyQueueTimestampQueriesSupported != FALSE));
    open(json, "options19", '{');
    member(json, "reported", jsonBool(probe.options19Reported));
    member(json, "maxSamplerDescriptorHeapSize", std::to_string(probe.options19.MaxSamplerDescriptorHeapSize));
    member(json, "maxSamplerDescriptorHeapSizeWithStaticSamplers", std::to_string(probe.options19.MaxSamplerDescriptorHeapSizeWithStaticSamplers));
    member(json, "maxViewDescriptorHeapSize", std::to_string(probe.options19.MaxViewDescriptorHeapSize));
    close(json, '}');
    member(json, "largestViewHeap", probe.largestViewHeap ? std::to_string(*probe.largestViewHeap) : "null");
    writeAttempts(json, "viewHeapAttempts", "size", probe.viewHeapAttempts);
    writeAttempts(json, "queries", "feature", probe.queries);
    close(json, '}');
}

//======================================================================================================================
static void writePresent(Json& json, const Probe& probe)
{
    const auto presented = [](HRESULT result) { return result == S_OK || result == DXGI_STATUS_OCCLUDED; };
    const bool acquired =
        probe.waits.size() == 2 && std::all_of(probe.waits.begin(), probe.waits.end(), [](const std::string& wait) { return wait == "signaled"; });
    open(json, "present", '{');
    member(json, "window", hresult(probe.windowResult));
    member(json, "queue", hresult(probe.queueResult));
    member(json, "swapchain", hresult(probe.swapchainResult));
    member(json, "acquired", jsonBool(acquired));
    member(json, "presented", jsonBool(probe.presentResults.size() == 2 && std::all_of(probe.presentResults.begin(), probe.presentResults.end(), presented)));
    open(json, "waits", '[');
    for (const std::string& wait : probe.waits)
    {
        member(json, "", quote(wait));
    }
    close(json, ']');
    open(json, "backBufferIndices", '[');
    for (const UINT index : probe.backBufferIndices)
    {
        member(json, "", std::to_string(index));
    }
    close(json, ']');
    open(json, "results", '[');
    for (const HRESULT result : probe.presentResults)
    {
        member(json, "", hresult(result));
    }
    close(json, ']');
    close(json, '}');
}

//======================================================================================================================
static std::string writeReport(Probe& probe, const std::vector<std::string>& missing)
{
    Json json;
    open(json, "", '{');
    member(json, "variant", quote(kVariant));
    member(json, "executableDir", quote(utf8(probe.exeDir)));

    open(json, "os", '{');
    member(json, "major", probe.os ? std::to_string(probe.os->dwMajorVersion) : "null");
    member(json, "minor", probe.os ? std::to_string(probe.os->dwMinorVersion) : "null");
    member(json, "build", probe.os ? std::to_string(probe.os->dwBuildNumber) : "null");
    member(json, "ubr", probe.ubr ? std::to_string(*probe.ubr) : "null");
    close(json, '}');

    open(json, "compiler", '{');
    member(json, "mscFullVer", std::to_string(_MSC_FULL_VER));
    member(json, "msvcLang", std::to_string(_MSVC_LANG));
    close(json, '}');

    open(json, "agility", '{');
    member(json, "method", quote(probe.agilityMethod));
    member(json, "sdkPath", quote(probe.sdkPath));
    writeModules(json, probe.agilityModules);
    member(json, "hresult", hresult(probe.agilityResult));
    member(json, "sdkConfigurationHresult", hresult(probe.sdkConfigResult));
    writeAttempts(json, "attempts", "sdkPath", probe.factoryAttempts);
    close(json, '}');

    open(json, "adapter", '{');
    member(json, "dxgiFactoryHresult", hresult(probe.dxgiFactoryResult));
    member(json, "hresult", hresult(probe.adapterResult));
    if (probe.adapterDesc)
    {
        member(json, "description", quote(utf8(probe.adapterDesc->Description)));
        member(json, "vendorId", std::to_string(probe.adapterDesc->VendorId));
        member(json, "deviceId", std::to_string(probe.adapterDesc->DeviceId));
        member(json, "flags", std::to_string(probe.adapterDesc->Flags));
    }
    close(json, '}');

    open(json, "device", '{');
    member(json, "created", jsonBool(probe.device.p != nullptr));
    member(json, "hresult", probe.deviceAttempts.empty() ? "null" : hresult(probe.deviceAttempts.front().result));
    member(json, "featureLevel", probe.deviceFeatureLevel ? quote(featureLevelName(*probe.deviceFeatureLevel)) : "null");
    member(json, "device12", jsonBool(probe.device12));
    writeAttempts(json, "attempts", "featureLevel", probe.deviceAttempts);
    close(json, '}');

    open(json, "warp", '{');
    writeModules(json, probe.warpModules);
    close(json, '}');

    open(json, "debug", '{');
    member(json, "layerRequested", jsonBool(probe.layerRequested));
    member(json, "layerActive", jsonBool(probe.layerActive));
    member(json, "gbvRequested", jsonBool(probe.gbvRequested));
    member(json, "gbvActive", jsonBool(probe.gbvActive));
    member(json, "dredConfigured", jsonBool(probe.dredConfigured));
    member(json, "dredActive", jsonBool(probe.dredActive));
    member(json, "infoQueue1Callback", jsonBool(probe.infoQueue1Callback));
    member(json, "debugHresult", hresult(probe.debugResult));
    member(json, "dredHresult", hresult(probe.dredResult));
    member(json, "infoQueue1Hresult", hresult(probe.infoQueue1Result));
    close(json, '}');

    writeFeatures(json, probe);

    open(json, "rawView", '{');
    member(json, "elements2p27Error", jsonOptionalBool(probe.rawViewErrors[0]));
    member(json, "elements2p27plus1Error", jsonOptionalBool(probe.rawViewErrors[1]));
    member(json, "bufferHresult", hresult(probe.rawBufferResult));
    member(json, "heapHresult", hresult(probe.rawHeapResult));
    close(json, '}');

    writePresent(json, probe);

    {
        std::lock_guard lock(probe.log.mutex);
        open(json, "messages", '[');
        for (const Message& message : probe.log.messages)
        {
            open(json, "", '{');
            member(json, "phase", quote(message.phase));
            member(json, "severity", quote(message.severity));
            member(json, "id", std::to_string(message.id));
            member(json, "text", quote(message.text));
            close(json, '}');
        }
        close(json, ']');
        open(json, "messageCounts", '{');
        for (const auto& [phase, count] : probe.log.counts)
        {
            member(json, phase, std::to_string(count));
        }
        close(json, '}');
    }

    open(json, "floor", '{');
    member(json, "met", jsonBool(missing.empty()));
    open(json, "missing", '[');
    for (const std::string& item : missing)
    {
        member(json, "", quote(item));
    }
    close(json, ']');
    close(json, '}');

    close(json, '}');
    return json.text;
}

//======================================================================================================================
// Exit codes: 0 when the floor is met on the app-local WARP, 1 when a device exists but either is not, 2 with no device.
int main()
{
    Probe probe;
    probeProcess(probe);
    loadAgility(probe);
    configureDebug(probe);
    createDevice(probe);
    findRuntimeModules(probe);
    if (probe.device.p != nullptr)
    {
        probeDebugStatus(probe);
        probeFeatures(probe);
        probeViewHeap(probe);
        probeRawViews(probe);
        probePresentation(probe);
        beginPhase(probe, "end");
        // Modules loaded by any later phase count too, so a second copy of either runtime is caught.
        findRuntimeModules(probe);
    }

    const std::vector<std::string> missing = missingFloorItems(probe);
    const std::string report = writeReport(probe, missing);
    std::fwrite(report.data(), 1, report.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);

    if (probe.infoQueue1Callback)
    {
        probe.infoQueue1.p->UnregisterMessageCallback(probe.callbackCookie);
    }

    const bool appLocal = warpAppLocal(probe);
    std::string floorText = "met";
    if (!missing.empty())
    {
        floorText = "missing";
        for (const std::string& item : missing)
        {
            floorText += " " + item;
        }
    }
    const LoadedModule* core = chosenModule(probe.agilityModules);
    std::fprintf(
        stderr,
        "d3d12 probe (%s): device %s, floor %s, WARP app-local %s, D3D12Core %s\n",
        kVariant,
        probe.device.p != nullptr ? "created" : "not created",
        floorText.c_str(),
        appLocal ? "yes" : "no",
        core != nullptr ? utf8(core->path).c_str() : "not loaded"
    );
    if (probe.device.p == nullptr)
    {
        return 2;
    }
    return missing.empty() && appLocal ? 0 : 1;
}
