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

#include <api/include/dx12/ffx_api_dx12.h>
#include <api/include/ffx_api.h>
#include <api/include/ffx_api_types.h>
#include <upscalers/include/ffx_upscale.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

constexpr UINT kSdkVersion = 619;
static_assert(D3D12_SDK_VERSION == kSdkVersion, "the Agility SDK headers are not version 619");
constexpr size_t kMessagesPerPhase = 64;
constexpr UINT64 kRawViewElements = UINT64{ 1 } << 27;
constexpr UINT kWindowSize = 256;
constexpr const wchar_t* kWindowClass = L"RojoRHID3D12Probe";
constexpr const char* kRelativeSdkPath = ".\\D3D12\\";
constexpr const wchar_t* kFsrLoader = L"amd_fidelityfx_loader_dx12.dll";
constexpr const wchar_t* kFsrUpscaler = L"amd_fidelityfx_upscaler_dx12.dll";
constexpr UINT kFsrRenderSize = 64;
constexpr UINT kFsrUpscaleSize = 128;
constexpr float kFsrFrameTimeMs = 16.6f;
// Half-float RGBA (0.5, 0.25, 0.125, 1.0), red in the low bits: the constant color of the FSR input.
constexpr uint64_t kFsrColorTexel = 0x3C00300034003800ull;
constexpr float kFsrDepth = 0.5f;
constexpr std::chrono::seconds kFsrTimeout{ 30 };
// Under GPU-based validation the FSR section loads the upscaler and creates a context but does not dispatch: on WARP the
// dispatch never completed within kFsrTimeout on a hosted runner and on some runs faulted inside the runtime while the
// probe waited for it (issue #96), so it carries no result. The upscale cases run on WARP only without it (PR #76).
constexpr const char* kFsrGbvDispatchSkipped =
    "skipped under GPU-based validation: on WARP the dispatch does not complete within the 30 s limit and has faulted inside the runtime; "
    "upscale cases run on WARP only without it";

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
// Besides the first messages of each phase, it keeps the messages of severity warning or worse, errors and corruption
// before warnings, and counts every one of them.
struct MessageLog
{
    std::mutex mutex;
    std::string phase = "start";
    std::vector<Message> messages;
    std::map<std::string, size_t> counts;
    std::map<std::string, size_t> errors;
    std::map<std::string, std::vector<Message>> severe;
    std::map<std::string, size_t> severeCounts;
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

// The FidelityFX API entry points the loader exports.
struct FfxApi
{
    PfnFfxCreateContext createContext = nullptr;
    PfnFfxDestroyContext destroyContext = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};

// What the FidelityFX upscaler section measured. It is no floor item: the floor verdict and the exit code ignore it.
struct FsrReport
{
    bool loaded = false;
    std::string version;
    std::vector<std::string> versions;
    bool contextCreated = false;
    bool dispatched = false;
    std::string dispatchSkipped;
    std::optional<uint64_t> outputTexel;
    std::string error;
    std::vector<LoadedModule> upscalerModules;
};

// One run of the FSR section on one device. Its thread writes report; under log->mutex it copies report to snapshot as
// each step begins and names the step, so a section that does not finish in time is reported from them.
struct FsrSection
{
    std::string phasePrefix;
    std::wstring exeDir;
    ID3D12Device* device = nullptr;
    MessageLog* log = nullptr;
    // Why the section does not dispatch, empty when it does.
    std::string_view skipDispatch;
    FsrReport report;
    FsrReport snapshot;
    std::string step;
    bool finished = true;
};

// The D3D12 objects the FSR section records and submits with, released when the section ends.
struct FsrRun
{
    Com<ID3D12Device10> device;
    Com<ID3D12CommandQueue> queue;
    Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList7> list;
    Com<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    Com<ID3D12Resource> color;
    Com<ID3D12Resource> depth;
    Com<ID3D12Resource> motion;
    Com<ID3D12Resource> output;
    Com<ID3D12Resource> readback;
    Com<ID3D12Resource> uploads[3];
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

    // FidelityFX upscaler: on the probe's device, and on a second device whose debug layer runs without GPU-based
    // validation
    FsrSection fsrGbv;
    FsrSection fsrNoGbv;
    std::optional<HRESULT> noGbvFactoryResult;
    std::optional<HRESULT> noGbvDebugResult;
    std::optional<HRESULT> noGbvDeviceResult;
    std::optional<HRESULT> noGbvCallbackResult;
    std::string noGbvStartup;

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
    MessageLog noGbvLog;
    Com<ID3D12DeviceFactory> noGbvFactory;
    Com<ID3D12Debug3> noGbvDebug;
    Com<ID3D12Device> noGbvDevice;
    Com<ID3D12InfoQueue1> noGbvInfoQueue1;
    DWORD noGbvCookie = 0;
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
    const bool error = severity == D3D12_MESSAGE_SEVERITY_ERROR || severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
    if (error)
    {
        ++messageLog.errors[messageLog.phase];
    }
    if (error || severity == D3D12_MESSAGE_SEVERITY_WARNING)
    {
        ++messageLog.severeCounts[messageLog.phase];
        std::vector<Message>& kept = messageLog.severe[messageLog.phase];
        Message message = { messageLog.phase, severityName(severity), id, text };
        if (kept.size() < kMessagesPerPhase)
        {
            kept.push_back(std::move(message));
        }
        else if (error)
        {
            // A full list gives up a warning for an error.
            const auto warning = std::find_if(kept.begin(), kept.end(), [](const Message& other) { return other.severity == "warning"; });
            if (warning != kept.end())
            {
                *warning = std::move(message);
            }
        }
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
// Files the messages the info queue stored under the log's current phase, then clears them.
static void drainInfoQueue(ID3D12InfoQueue* queue, MessageLog& messageLog)
{
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
            recordMessage(messageLog, message->Severity, static_cast<int>(message->ID), fromAnsi(message->pDescription));
        }
    }
    queue->ClearStoredMessages();
}

//======================================================================================================================
// Without a message callback, the messages the info queue stored are read at each phase boundary instead.
static void drainStoredMessages(Probe& probe)
{
    if (probe.infoQueue.p != nullptr && !probe.infoQueue1Callback)
    {
        drainInfoQueue(probe.infoQueue.p, probe.log);
    }
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
// Messages the section's device raises from here on are filed under this phase of the section's log.
static void beginSectionPhase(FsrSection& section, std::string_view phase)
{
    const std::string name = section.phasePrefix + std::string(phase);
    {
        std::lock_guard lock(section.log->mutex);
        section.log->phase = name;
    }
    std::fprintf(stderr, "d3d12 probe (%s): %s\n", kVariant, name.c_str());
}

//======================================================================================================================
// Marks the call the FSR section makes next, on stderr and in the snapshot a section that does not finish reports.
static void beginFsrStep(FsrSection& section, std::string_view step)
{
    {
        std::lock_guard lock(section.log->mutex);
        section.snapshot = section.report;
        section.step = step;
    }
    std::fprintf(stderr, "d3d12 probe (%s): %s step %.*s\n", kVariant, section.phasePrefix.c_str(), static_cast<int>(step.size()), step.data());
}

//======================================================================================================================
// Records the first failing call of the FSR section; a later failure keeps the first.
static bool succeeded(FsrSection& section, std::string_view call, HRESULT result)
{
    if (FAILED(result) && section.report.error.empty())
    {
        section.report.error = std::format("{} 0x{:08X}", call, static_cast<uint32_t>(result));
    }
    return SUCCEEDED(result);
}

//======================================================================================================================
static const char* ffxReturnCodeName(ffxReturnCode_t code)
{
    switch (code)
    {
    case FFX_API_RETURN_OK: return "FFX_API_RETURN_OK";
    case FFX_API_RETURN_ERROR: return "FFX_API_RETURN_ERROR";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "FFX_API_RETURN_ERROR_RUNTIME_ERROR";
    case FFX_API_RETURN_NO_PROVIDER: return "FFX_API_RETURN_NO_PROVIDER";
    case FFX_API_RETURN_ERROR_MEMORY: return "FFX_API_RETURN_ERROR_MEMORY";
    case FFX_API_RETURN_ERROR_PARAMETER: return "FFX_API_RETURN_ERROR_PARAMETER";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
    }
    return "unknown";
}

//======================================================================================================================
static bool ffxSucceeded(FsrSection& section, std::string_view call, ffxReturnCode_t code)
{
    const ffxReturnCode_t ok = FFX_API_RETURN_OK;
    if (code != ok && section.report.error.empty())
    {
        section.report.error = std::format("{} {} ({})", call, ffxReturnCodeName(code), code);
    }
    return code == ok;
}

//======================================================================================================================
template<typename T> static bool resolveFfx(FsrSection& section, HMODULE loader, const char* name, T& entry)
{
    entry = reinterpret_cast<T>(GetProcAddress(loader, name));
    if (entry == nullptr && section.report.error.empty())
    {
        section.report.error = std::format("GetProcAddress {} 0x{:08X}", name, static_cast<uint32_t>(HRESULT_FROM_WIN32(GetLastError())));
    }
    return entry != nullptr;
}

//======================================================================================================================
// The upscaler versions the loader offers for the device; the first is the one a context gets by default.
static bool queryFsrVersions(FsrSection& section, const FfxApi& api)
{
    beginFsrStep(section, "ffxQuery (GET_VERSIONS)");
    uint64_t count = 0;
    ffxQueryDescGetVersions query = {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.device = section.device;
    query.outputCount = &count;
    if (!ffxSucceeded(section, "ffxQuery (GET_VERSIONS count)", api.query(nullptr, &query.header)))
    {
        return false;
    }
    std::vector<uint64_t> ids(static_cast<size_t>(count));
    std::vector<const char*> names(static_cast<size_t>(count), nullptr);
    query.versionIds = ids.data();
    query.versionNames = names.data();
    if (count > 0 && !ffxSucceeded(section, "ffxQuery (GET_VERSIONS)", api.query(nullptr, &query.header)))
    {
        return false;
    }
    names.resize(static_cast<size_t>(std::min<uint64_t>(count, names.size())));
    for (const char* name : names)
    {
        section.report.versions.push_back(name != nullptr ? name : "");
    }
    if (section.report.versions.empty())
    {
        section.report.error = "ffxQuery (GET_VERSIONS) found no upscaler provider for the device";
        return false;
    }
    section.report.version = section.report.versions.front();
    return true;
}

//======================================================================================================================
static D3D12_RESOURCE_DESC1 textureDesc(UINT size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC1 desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

//======================================================================================================================
static D3D12_RESOURCE_DESC1 bufferDesc(UINT64 size)
{
    D3D12_RESOURCE_DESC1 desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

//======================================================================================================================
static bool createCommitted(
    FsrSection& section,
    FsrRun& run,
    std::string_view what,
    D3D12_HEAP_TYPE heap,
    const D3D12_RESOURCE_DESC1& desc,
    D3D12_BARRIER_LAYOUT layout,
    Com<ID3D12Resource>& out
)
{
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = heap;
    const HRESULT result =
        run.device.p->CreateCommittedResource3(&properties, D3D12_HEAP_FLAG_NONE, &desc, layout, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&out.p));
    return succeeded(section, std::format("CreateCommittedResource3 ({})", what), result);
}

//======================================================================================================================
// A layout transition at the start of a submission: the work before it finished at the previous fence wait.
static D3D12_TEXTURE_BARRIER textureBarrier(
    ID3D12Resource* resource, D3D12_BARRIER_LAYOUT before, D3D12_BARRIER_LAYOUT after, D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access
)
{
    D3D12_TEXTURE_BARRIER barrier = {};
    barrier.SyncBefore = D3D12_BARRIER_SYNC_NONE;
    barrier.SyncAfter = sync;
    barrier.AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
    barrier.AccessAfter = access;
    barrier.LayoutBefore = before;
    barrier.LayoutAfter = after;
    barrier.pResource = resource;
    barrier.Subresources = { 0, 1, 0, 1, 0, 1 };
    barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
    return barrier;
}

//======================================================================================================================
// The queue, list, fence, the three inputs in COPY_DEST, the output in COMMON and the readback buffer. Sampled depth is
// R32_TYPELESS, as the backend creates it, and FidelityFX reads it through an R32_FLOAT view.
static bool createFsrObjects(FsrSection& section, FsrRun& run)
{
    if (!succeeded(section, "QueryInterface (ID3D12Device10)", queryInterface(section.device, run.device)))
    {
        return false;
    }
    ID3D12Device10* device = run.device.p;
    const D3D12_COMMAND_LIST_TYPE direct = D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = direct;
    const bool commands =
        succeeded(section, "CreateCommandQueue", device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&run.queue.p))) &&
        succeeded(section, "CreateCommandAllocator", device->CreateCommandAllocator(direct, IID_PPV_ARGS(&run.allocator.p))) &&
        succeeded(section, "CreateCommandList1", device->CreateCommandList1(0, direct, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&run.list.p))) &&
        succeeded(section, "CreateFence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&run.fence.p)));
    if (!commands)
    {
        return false;
    }
    const D3D12_HEAP_TYPE gpu = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_BARRIER_LAYOUT copyDest = D3D12_BARRIER_LAYOUT_COPY_DEST;
    const D3D12_RESOURCE_DESC1 color = textureDesc(kFsrRenderSize, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC1 depth = textureDesc(kFsrRenderSize, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const D3D12_RESOURCE_DESC1 motion = textureDesc(kFsrRenderSize, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC1 output = textureDesc(kFsrUpscaleSize, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC1 readback = bufferDesc(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    return createCommitted(section, run, "color", gpu, color, copyDest, run.color) && createCommitted(section, run, "depth", gpu, depth, copyDest, run.depth) &&
           createCommitted(section, run, "motion", gpu, motion, copyDest, run.motion) &&
           createCommitted(section, run, "output", gpu, output, D3D12_BARRIER_LAYOUT_COMMON, run.output) &&
           createCommitted(section, run, "readback", D3D12_HEAP_TYPE_READBACK, readback, D3D12_BARRIER_LAYOUT_UNDEFINED, run.readback);
}

//======================================================================================================================
static bool beginList(FsrSection& section, FsrRun& run)
{
    return succeeded(section, "ID3D12CommandAllocator::Reset", run.allocator.p->Reset()) &&
           succeeded(section, "ID3D12GraphicsCommandList::Reset", run.list.p->Reset(run.allocator.p, nullptr));
}

//======================================================================================================================
// Executes the open list and waits for it; a removed device fails the submission.
static bool submitAndWait(FsrSection& section, FsrRun& run, std::string_view what)
{
    if (!succeeded(section, std::format("Close ({})", what), run.list.p->Close()))
    {
        return false;
    }
    ID3D12CommandList* lists[] = { run.list.p };
    run.queue.p->ExecuteCommandLists(1, lists);
    ++run.fenceValue;
    return succeeded(section, std::format("Signal ({})", what), run.queue.p->Signal(run.fence.p, run.fenceValue)) &&
           succeeded(section, std::format("SetEventOnCompletion ({})", what), run.fence.p->SetEventOnCompletion(run.fenceValue, nullptr)) &&
           succeeded(section, std::format("GetDeviceRemovedReason ({})", what), section.device->GetDeviceRemovedReason());
}

//======================================================================================================================
// Fills an upload buffer with one texel repeated over the whole texture and records its copy into the texture.
static bool recordUpload(FsrSection& section, FsrRun& run, ID3D12Resource* texture, const void* texel, size_t texelSize, Com<ID3D12Resource>& upload)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 total = 0;
    section.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    if (!createCommitted(section, run, "upload", D3D12_HEAP_TYPE_UPLOAD, bufferDesc(total), D3D12_BARRIER_LAYOUT_UNDEFINED, upload))
    {
        return false;
    }
    const D3D12_RANGE noRead = { 0, 0 };
    void* mapped = nullptr;
    if (!succeeded(section, "Map (upload)", upload.p->Map(0, &noRead, &mapped)))
    {
        return false;
    }
    auto* bytes = static_cast<std::byte*>(mapped) + footprint.Offset;
    for (UINT row = 0; row < rows; ++row)
    {
        for (UINT64 column = 0; column < desc.Width; ++column)
        {
            std::memcpy(bytes + static_cast<size_t>(row) * footprint.Footprint.RowPitch + column * texelSize, texel, texelSize);
        }
    }
    upload.p->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = upload.p;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    run.list.p->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    return true;
}

//======================================================================================================================
// The color input to a constant color, depth to a constant depth and the motion vectors to zero, in one submission.
static bool uploadFsrInputs(FsrSection& section, FsrRun& run)
{
    const uint32_t depth = std::bit_cast<uint32_t>(kFsrDepth);
    const uint32_t motion = 0;
    return beginList(section, run) && recordUpload(section, run, run.color.p, &kFsrColorTexel, sizeof(kFsrColorTexel), run.uploads[0]) &&
           recordUpload(section, run, run.depth.p, &depth, sizeof(depth), run.uploads[1]) &&
           recordUpload(section, run, run.motion.p, &motion, sizeof(motion), run.uploads[2]) && submitAndWait(section, run, "upload");
}

//======================================================================================================================
// A minimal upscale context: no flags, 64x64 render size and 128x128 upscale size, on the section's device.
static bool createFsrContext(FsrSection& section, const FfxApi& api, ffxContext& context)
{
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = section.device;
    ffxCreateContextDescUpscaleVersion version = {};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.header.pNext = &backend.header;
    version.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale upscale = {};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.header.pNext = &version.header;
    upscale.flags = 0;
    upscale.maxRenderSize = { kFsrRenderSize, kFsrRenderSize };
    upscale.maxUpscaleSize = { kFsrUpscaleSize, kFsrUpscaleSize };
    upscale.fpMessage = nullptr;
    beginFsrStep(section, "ffxCreateContext");
    if (!ffxSucceeded(section, "ffxCreateContext", api.createContext(&context, &upscale.header, nullptr)))
    {
        return false;
    }
    section.report.contextCreated = true;
    // The provider the context runs on, which names the version better than the default the version query reported.
    ffxQueryGetProviderVersion provider = {};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    beginFsrStep(section, "ffxQuery (GET_PROVIDER_VERSION)");
    if (ffxSucceeded(section, "ffxQuery (GET_PROVIDER_VERSION)", api.query(&context, &provider.header)) && provider.versionName != nullptr)
    {
        section.report.version = provider.versionName;
    }
    return true;
}

//======================================================================================================================
static FfxApiResource fsrResource(ID3D12Resource* resource, UINT size, uint32_t format, uint32_t usage, uint32_t state)
{
    FfxApiResource out = {};
    out.resource = resource;
    out.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    out.description.format = format;
    out.description.width = size;
    out.description.height = size;
    out.description.depth = 1;
    out.description.mipCount = 1;
    out.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    out.description.usage = usage;
    out.state = state;
    return out;
}

//======================================================================================================================
// One direct list: an enhanced barrier group moving the inputs to SHADER_RESOURCE and the output to UNORDERED_ACCESS,
// then the upscale, which FidelityFX records with legacy barriers from the states it is given: the inputs in
// NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE, the output in UNORDERED_ACCESS.
static bool dispatchFsr(FsrSection& section, const FfxApi& api, FsrRun& run, ffxContext& context)
{
    if (!beginList(section, run))
    {
        return false;
    }
    const D3D12_BARRIER_LAYOUT copyDest = D3D12_BARRIER_LAYOUT_COPY_DEST;
    const D3D12_BARRIER_LAYOUT shaderResource = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    const D3D12_BARRIER_SYNC compute = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    const D3D12_BARRIER_ACCESS read = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    const D3D12_TEXTURE_BARRIER barriers[] = {
        textureBarrier(run.color.p, copyDest, shaderResource, compute, read),
        textureBarrier(run.depth.p, copyDest, shaderResource, compute, read),
        textureBarrier(run.motion.p, copyDest, shaderResource, compute, read),
        textureBarrier(run.output.p, D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, compute, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS),
    };
    D3D12_BARRIER_GROUP group = {};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = static_cast<UINT32>(std::size(barriers));
    group.pTextureBarriers = barriers;
    run.list.p->Barrier(1, &group);

    const uint32_t inputState = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    ffxDispatchDescUpscale desc = {};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    desc.commandList = run.list.p;
    desc.color = fsrResource(run.color.p, kFsrRenderSize, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY, inputState);
    desc.depth = fsrResource(run.depth.p, kFsrRenderSize, FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_USAGE_DEPTHTARGET, inputState);
    desc.motionVectors = fsrResource(run.motion.p, kFsrRenderSize, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY, inputState);
    desc.output = fsrResource(
        run.output.p, kFsrUpscaleSize, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
    );
    desc.jitterOffset = { 0.0f, 0.0f };
    desc.motionVectorScale = { static_cast<float>(kFsrRenderSize), static_cast<float>(kFsrRenderSize) };
    desc.renderSize = { kFsrRenderSize, kFsrRenderSize };
    desc.upscaleSize = { kFsrUpscaleSize, kFsrUpscaleSize };
    desc.enableSharpening = false;
    desc.sharpness = 0.0f;
    desc.frameTimeDelta = kFsrFrameTimeMs;
    desc.preExposure = 1.0f;
    desc.reset = true;
    desc.cameraNear = 0.1f;
    desc.cameraFar = 100.0f;
    desc.cameraFovAngleVertical = 1.0f;
    desc.viewSpaceToMetersFactor = 1.0f;
    desc.flags = 0;
    beginFsrStep(section, "ffxDispatch");
    if (!ffxSucceeded(section, "ffxDispatch", api.dispatch(&context, &desc.header)))
    {
        return false;
    }
    beginFsrStep(section, "dispatch submission and wait");
    section.report.dispatched = submitAndWait(section, run, "dispatch");
    return section.report.dispatched;
}

//======================================================================================================================
// Copies the output's centre texel to the readback buffer and reads it.
static void readFsrOutput(FsrSection& section, FsrRun& run)
{
    beginFsrStep(section, "readback");
    if (!beginList(section, run))
    {
        return;
    }
    const D3D12_TEXTURE_BARRIER barrier = textureBarrier(
        run.output.p, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_COPY_SOURCE, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE
    );
    D3D12_BARRIER_GROUP group = {};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &barrier;
    run.list.p->Barrier(1, &group);
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = run.output.p;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = run.readback.p;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Offset = 0;
    destination.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
    const UINT centre = kFsrUpscaleSize / 2;
    const D3D12_BOX box = { centre, centre, 0, centre + 1, centre + 1, 1 };
    run.list.p->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
    if (!submitAndWait(section, run, "readback"))
    {
        return;
    }
    const D3D12_RANGE read = { 0, sizeof(uint64_t) };
    void* mapped = nullptr;
    if (!succeeded(section, "Map (readback)", run.readback.p->Map(0, &read, &mapped)))
    {
        return;
    }
    uint64_t texel = 0;
    std::memcpy(&texel, mapped, sizeof(texel));
    const D3D12_RANGE noWrite = { 0, 0 };
    run.readback.p->Unmap(0, &noWrite);
    section.report.outputTexel = texel;
}

//======================================================================================================================
// The version query, the inputs, then one context: created, dispatched once unless the section skips the dispatch, read
// back and destroyed. The Dispatch phase runs from the context's creation to the end of the dispatch's wait.
static void runFsr(FsrSection& section, const FfxApi& api)
{
    if (!queryFsrVersions(section, api))
    {
        return;
    }
    beginSectionPhase(section, "Setup");
    FsrRun run;
    beginFsrStep(section, "resource creation");
    if (!createFsrObjects(section, run))
    {
        return;
    }
    beginFsrStep(section, "input upload");
    if (!uploadFsrInputs(section, run))
    {
        return;
    }
    beginSectionPhase(section, "Dispatch");
    ffxContext context = nullptr;
    if (!createFsrContext(section, api, context))
    {
        return;
    }
    if (!section.skipDispatch.empty())
    {
        section.report.dispatchSkipped = section.skipDispatch;
    }
    const bool dispatched = section.skipDispatch.empty() && dispatchFsr(section, api, run, context);
    if (dispatched)
    {
        beginSectionPhase(section, "Readback");
        readFsrOutput(section, run);
    }
    beginSectionPhase(section, "Teardown");
    beginFsrStep(section, "ffxDestroyContext");
    ffxSucceeded(section, "ffxDestroyContext", api.destroyContext(&context, nullptr));
}

//======================================================================================================================
// FidelityFX FSR through the loader beside the executable, on the section's device.
static void probeFsr(FsrSection& section)
{
    beginSectionPhase(section, "Load");
    beginFsrStep(section, "LoadLibraryW");
    const HMODULE loader = LoadLibraryW((section.exeDir + L"\\" + kFsrLoader).c_str());
    if (loader == nullptr)
    {
        section.report.error = std::format("LoadLibraryW {} 0x{:08X}", utf8(kFsrLoader), static_cast<uint32_t>(HRESULT_FROM_WIN32(GetLastError())));
        return;
    }
    FfxApi api;
    const bool resolved = resolveFfx(section, loader, "ffxCreateContext", api.createContext) &&
                          resolveFfx(section, loader, "ffxDestroyContext", api.destroyContext) && resolveFfx(section, loader, "ffxConfigure", api.configure) &&
                          resolveFfx(section, loader, "ffxQuery", api.query) && resolveFfx(section, loader, "ffxDispatch", api.dispatch);
    if (resolved)
    {
        section.report.loaded = true;
        runFsr(section, api);
    }
    section.report.upscalerModules = loadedModules(kFsrUpscaler, section.exeDir);
    beginFsrStep(section, "FreeLibrary");
    FreeLibrary(loader);
    beginFsrStep(section, "done");
}

//======================================================================================================================
// Runs the FSR section on a thread of its own and waits for it at most kFsrTimeout, so a call that never returns is
// reported instead of stopping the probe. Returns whether the section finished; when it did not, its thread keeps
// running and the process must end without releasing what the section uses.
static bool probeFsrWithin(FsrSection& section)
{
    auto finished = std::make_shared<std::promise<void>>();
    std::future<void> done = finished->get_future();
    std::thread worker([&section, finished] {
        probeFsr(section);
        finished->set_value();
    });
    if (done.wait_for(kFsrTimeout) == std::future_status::ready)
    {
        worker.join();
        return true;
    }
    worker.detach();
    section.finished = false;
    return false;
}

//======================================================================================================================
// The report of an FSR section that did not finish: what it had measured when its last step began.
static FsrReport unfinishedFsr(FsrSection& section)
{
    std::lock_guard lock(section.log->mutex);
    FsrReport fsr = section.snapshot;
    if (fsr.error.empty())
    {
        fsr.error = std::format("{} did not return within {} s", section.step, kFsrTimeout.count());
    }
    return fsr;
}

//======================================================================================================================
// A second device on the WARP adapter, from a device factory of its own whose debug layer runs without GPU-based
// validation. With no factory flag it is independent of the probe's device; its messages go to a log of their own.
static void createNoGbvDevice(Probe& probe)
{
    {
        std::lock_guard lock(probe.noGbvLog.mutex);
        probe.noGbvLog.phase = "fsrNoGbvDevice";
    }
    if constexpr (kExportsVariant)
    {
        probe.noGbvFactoryResult = D3D12GetInterface(CLSID_D3D12DeviceFactory, IID_PPV_ARGS(&probe.noGbvFactory.p));
    }
    else
    {
        if (probe.sdkConfig.p == nullptr)
        {
            return;
        }
        probe.noGbvFactoryResult = probe.sdkConfig.p->CreateDeviceFactory(kSdkVersion, probe.sdkPath.c_str(), IID_PPV_ARGS(&probe.noGbvFactory.p));
    }
    if (FAILED(*probe.noGbvFactoryResult))
    {
        return;
    }
    probe.noGbvDebugResult = probe.noGbvFactory.p->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&probe.noGbvDebug.p));
    if (FAILED(*probe.noGbvDebugResult))
    {
        return;
    }
    probe.noGbvDebug.p->EnableDebugLayer();
    probe.noGbvDebug.p->SetEnableGPUBasedValidation(FALSE);
    probe.noGbvDeviceResult = probe.noGbvFactory.p->SetFlags(D3D12_DEVICE_FACTORY_FLAG_NONE);
    if (SUCCEEDED(*probe.noGbvDeviceResult))
    {
        probe.noGbvDeviceResult = probe.noGbvFactory.p->CreateDevice(probe.adapter.p, D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&probe.noGbvDevice.p));
    }
    if (FAILED(*probe.noGbvDeviceResult))
    {
        return;
    }
    // The debug layer's startup message, raised at device creation, says whether GPU-based validation is on.
    Com<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(queryInterface(probe.noGbvDevice.p, infoQueue)))
    {
        drainInfoQueue(infoQueue.p, probe.noGbvLog);
    }
    {
        std::lock_guard lock(probe.noGbvLog.mutex);
        for (const Message& message : probe.noGbvLog.messages)
        {
            if (message.id == static_cast<int>(D3D12_MESSAGE_ID_CREATEDEVICE_DEBUG_LAYER_STARTUP_OPTIONS))
            {
                probe.noGbvStartup = message.text;
            }
        }
    }
    probe.noGbvCallbackResult = queryInterface(probe.noGbvDevice.p, probe.noGbvInfoQueue1);
    if (SUCCEEDED(*probe.noGbvCallbackResult))
    {
        probe.noGbvCallbackResult =
            probe.noGbvInfoQueue1.p->RegisterMessageCallback(onMessage, D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, &probe.noGbvLog, &probe.noGbvCookie);
    }
}

//======================================================================================================================
// The section without GPU-based validation runs first: a section that does not finish leaves its work running on WARP,
// which would slow or stall any section after it. Returns whether both finished.
static bool probeFsrSections(Probe& probe)
{
    probe.fsrGbv.phasePrefix = "fsr";
    probe.fsrGbv.exeDir = probe.exeDir;
    probe.fsrGbv.device = probe.device.p;
    probe.fsrGbv.log = &probe.log;
    probe.fsrGbv.skipDispatch = kFsrGbvDispatchSkipped;
    probe.fsrNoGbv.phasePrefix = "fsrNoGbv";
    probe.fsrNoGbv.exeDir = probe.exeDir;
    probe.fsrNoGbv.log = &probe.noGbvLog;
    createNoGbvDevice(probe);
    bool finished = true;
    if (probe.noGbvDevice.p != nullptr)
    {
        probe.fsrNoGbv.device = probe.noGbvDevice.p;
        finished = probeFsrWithin(probe.fsrNoGbv);
    }
    else
    {
        probe.fsrNoGbv.report.error = "no device without GPU-based validation";
    }
    return probeFsrWithin(probe.fsrGbv) && finished;
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
// The fields of one FSR section. layerMessages holds the section's debug-layer messages of severity warning or worse,
// errors and corruption first, at most kMessagesPerPhase a phase, and layerMessageCount counts all of them by phase;
// dispatchSkipped says why the section skipped the dispatch and is null otherwise; outputNonZero is null when no texel
// was read.
static void writeFsrFields(Json& json, const FsrReport& fsr, MessageLog& messageLog, std::string_view phasePrefix)
{
    // A section that never ran has no phases.
    const auto ownPhase = [phasePrefix](const std::string& phase) { return !phasePrefix.empty() && phase.starts_with(phasePrefix); };
    member(json, "loaded", jsonBool(fsr.loaded));
    member(json, "version", fsr.version.empty() ? "null" : quote(fsr.version));
    open(json, "versions", '[');
    for (const std::string& version : fsr.versions)
    {
        member(json, "", quote(version));
    }
    close(json, ']');
    member(json, "contextCreated", jsonBool(fsr.contextCreated));
    member(json, "dispatched", jsonBool(fsr.dispatched));
    member(json, "dispatchSkipped", fsr.dispatchSkipped.empty() ? "null" : quote(fsr.dispatchSkipped));
    member(json, "outputNonZero", fsr.outputTexel ? jsonBool(*fsr.outputTexel != 0) : "null");
    member(json, "outputTexel", fsr.outputTexel ? quote(std::format("0x{:016X}", *fsr.outputTexel)) : "null");
    {
        std::lock_guard lock(messageLog.mutex);
        std::vector<Message> severe;
        for (const auto& [phase, kept] : messageLog.severe)
        {
            if (ownPhase(phase))
            {
                severe.insert(severe.end(), kept.begin(), kept.end());
            }
        }
        std::stable_sort(severe.begin(), severe.end(), [](const Message& a, const Message& b) { return a.severity != "warning" && b.severity == "warning"; });
        open(json, "layerMessages", '[');
        for (const Message& message : severe)
        {
            open(json, "", '{');
            member(json, "phase", quote(message.phase));
            member(json, "severity", quote(message.severity));
            member(json, "id", std::to_string(message.id));
            member(json, "text", quote(message.text));
            close(json, '}');
        }
        close(json, ']');
        open(json, "layerMessageCount", '{');
        for (const auto& [phase, count] : messageLog.severeCounts)
        {
            if (ownPhase(phase))
            {
                member(json, phase, std::to_string(count));
            }
        }
        close(json, '}');
    }
    member(json, "error", fsr.error.empty() ? "null" : quote(fsr.error));
    open(json, "upscaler", '{');
    writeModules(json, fsr.upscalerModules);
    close(json, '}');
}

//======================================================================================================================
// The section on the probe's device, with GPU-based validation, and in noGbv the one on the second device without it.
static void writeFsr(Json& json, Probe& probe, const FsrReport& gbv, const FsrReport& noGbv)
{
    open(json, "fsr", '{');
    writeFsrFields(json, gbv, probe.log, probe.fsrGbv.phasePrefix);
    open(json, "noGbv", '{');
    open(json, "device", '{');
    member(json, "created", jsonBool(probe.noGbvDevice.p != nullptr));
    member(json, "factoryHresult", hresult(probe.noGbvFactoryResult));
    member(json, "debugHresult", hresult(probe.noGbvDebugResult));
    member(json, "hresult", hresult(probe.noGbvDeviceResult));
    member(json, "messageCallbackHresult", hresult(probe.noGbvCallbackResult));
    member(json, "startupOptions", probe.noGbvStartup.empty() ? "null" : quote(probe.noGbvStartup));
    member(json, "gbvActive", jsonBool(probe.noGbvStartup.find("GPU-Based Validation is enabled") != std::string::npos));
    close(json, '}');
    // Messages of every severity by phase: a count above zero shows the debug layer ran on the second device.
    {
        std::lock_guard lock(probe.noGbvLog.mutex);
        open(json, "messageCounts", '{');
        for (const auto& [phase, count] : probe.noGbvLog.counts)
        {
            member(json, phase, std::to_string(count));
        }
        close(json, '}');
    }
    writeFsrFields(json, noGbv, probe.noGbvLog, probe.fsrNoGbv.phasePrefix);
    close(json, '}');
    close(json, '}');
}

//======================================================================================================================
static std::string writeReport(Probe& probe, const std::vector<std::string>& missing, const FsrReport& gbv, const FsrReport& noGbv)
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
    writeFsr(json, probe, gbv, noGbv);

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
static void printFsrSummary(const char* name, const FsrReport& fsr)
{
    const auto yesNo = [](bool value) { return value ? "yes" : "no"; };
    std::fprintf(
        stderr,
        "d3d12 probe (%s): %s loaded %s, version %s, context %s, dispatched %s, output non-zero %s, error %s\n",
        kVariant,
        name,
        yesNo(fsr.loaded),
        fsr.version.empty() ? "none" : fsr.version.c_str(),
        yesNo(fsr.contextCreated),
        fsr.dispatchSkipped.empty() ? yesNo(fsr.dispatched) : "skipped",
        fsr.outputTexel ? yesNo(*fsr.outputTexel != 0) : "not read",
        fsr.error.empty() ? "none" : fsr.error.c_str()
    );
}

//======================================================================================================================
// Exit codes: 0 when the floor is met on the app-local WARP, 1 when a device exists but either is not, 2 with no device.
// The FSR section never changes them. When it does not finish, the process is terminated without releasing anything or
// detaching any DLL, since its thread may still be inside a call on the device.
int main()
{
    Probe probe;
    bool fsrFinished = true;
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
        fsrFinished = probeFsrSections(probe);
        if (fsrFinished)
        {
            beginPhase(probe, "end");
        }
        // Modules loaded by any later phase count too, so a second copy of either runtime is caught.
        findRuntimeModules(probe);
    }

    const std::vector<std::string> missing = missingFloorItems(probe);
    const FsrReport gbv = probe.fsrGbv.finished ? probe.fsrGbv.report : unfinishedFsr(probe.fsrGbv);
    const FsrReport noGbv = probe.fsrNoGbv.finished ? probe.fsrNoGbv.report : unfinishedFsr(probe.fsrNoGbv);
    const std::string report = writeReport(probe, missing, gbv, noGbv);
    std::fwrite(report.data(), 1, report.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);

    if (probe.infoQueue1Callback && fsrFinished)
    {
        probe.infoQueue1.p->UnregisterMessageCallback(probe.callbackCookie);
    }
    if (probe.noGbvCallbackResult && SUCCEEDED(*probe.noGbvCallbackResult) && fsrFinished)
    {
        probe.noGbvInfoQueue1.p->UnregisterMessageCallback(probe.noGbvCookie);
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
    printFsrSummary("fsr (GBV)", gbv);
    printFsrSummary("fsr (no GBV)", noGbv);
    const int code = probe.device.p == nullptr ? 2 : (missing.empty() && appLocal ? 0 : 1);
    if (!fsrFinished)
    {
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    }
    return code;
}
