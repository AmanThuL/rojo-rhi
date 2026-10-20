// Spike 1 host: native D3D12 through the Agility SDK, no RojoRHI code. Throwaway spike source; see
// docs/research/2026-10-20-spike-1-d3d12-addr-dereference.md for what it measures and why.
//
//   Harness limits --adapter hardware|warp
//   Harness run --adapter A --kernels DIR --threads N --variant fetch|alu --order K
//               --mode correctness|timing --out FILE.json [--corrupt-data]
//   Harness viewcap --adapter A --out FILE.json
//
// `limits` prints one JSON object describing the device and the runtime that loaded. `run` runs the
// 12 kernels of one variant once each under the debug layer and GPU-based validation, compares
// every output byte with a CPU reference and writes the result to FILE.json; it exits non-zero on
// any mismatch or any message of severity warning or worse, which it also prints on standard
// error. `--corrupt-data` changes word 0 of elements 0 to 63 and of element 64k of every slice k
// in the GPU copy of the data after the reference is computed, so every kernel must mismatch.
// `viewcap` asks the debug layer about one raw view of 2^27 + 1 elements and writes what it said
// to FILE.json. Every other error prints one line on standard error and exits non-zero; success
// writes nothing there. `--mode timing` runs with the debug layer off: per kernel one command list
// of 250 dispatches, each between two timestamps, a UAV barrier between dispatches; it keeps the
// last 200 durations in nanoseconds, compares the output once afterwards and records the AC power
// state at its start and end.
#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// The Agility SDK's classic exports: the system D3D12 loader reads them from the executable and loads
// D3D12Core.dll of this SDK version from the D3D12 directory beside it.
static_assert(D3D12_SDK_VERSION == 619, "the harness pins Agility SDK 1.619");
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace {

    using Microsoft::WRL::ComPtr;

    enum class AdapterKind
    {
        Hardware,
        Warp
    };

    std::string narrow(const std::wstring& wide)
    {
        if (wide.empty())
            return {};
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), bytes, nullptr, nullptr);
        return out;
    }

    std::string hex(uint64_t value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
        return text;
    }

    void check(HRESULT hr, const std::string& what)
    {
        if (FAILED(hr))
            throw std::runtime_error(what + " failed with HRESULT " + hex(static_cast<uint32_t>(hr)));
    }

    std::string jsonString(const std::string& text)
    {
        std::string out = "\"";
        for (char c : text)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                    out += escaped;
                }
                else
                {
                    out += c;
                }
            }
        }
        return out + "\"";
    }

    std::filesystem::path executableDirectory()
    {
        std::wstring path(32768, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
            throw std::runtime_error("GetModuleFileNameW of the executable failed");
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    // The full path of every module loaded in this process whose file name is `name`, compared without case.
    std::vector<std::filesystem::path> loadedModules(const std::wstring& name)
    {
        std::vector<HMODULE> modules(1024);
        DWORD needed = 0;
        for (;;)
        {
            DWORD bytes = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
            if (!EnumProcessModules(GetCurrentProcess(), modules.data(), bytes, &needed))
                throw std::runtime_error("EnumProcessModules failed");
            if (needed <= bytes)
                break;
            modules.resize(needed / sizeof(HMODULE));
        }
        modules.resize(needed / sizeof(HMODULE));
        std::vector<std::filesystem::path> found;
        for (HMODULE module : modules)
        {
            std::wstring path(32768, L'\0');
            DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0 || length >= path.size())
                continue;
            path.resize(length);
            std::filesystem::path file(path);
            if (CompareStringOrdinal(file.filename().c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL)
                found.push_back(file);
        }
        return found;
    }

    // The one loaded copy of `name`, which must sit in `directory`; any other copy, or none, is an error,
    // because then the runtime under test is not the pinned one.
    std::filesystem::path requireLoadedFrom(const std::wstring& name, const std::filesystem::path& directory)
    {
        std::vector<std::filesystem::path> found = loadedModules(name);
        if (found.empty())
            throw std::runtime_error(narrow(name) + " is not loaded");
        for (const std::filesystem::path& file : found)
        {
            std::error_code error;
            bool same = std::filesystem::equivalent(file.parent_path(), directory, error);
            if (error)
                throw std::runtime_error(
                    "comparing " + narrow(file.parent_path().wstring()) + " with " + narrow(directory.wstring()) + " failed: " + error.message()
                );
            if (!same)
                throw std::runtime_error(narrow(name) + " loaded from " + narrow(file.wstring()) + ", not from " + narrow(directory.wstring()));
        }
        if (found.size() != 1)
            throw std::runtime_error(narrow(name) + " is loaded more than once");
        return found.front();
    }

    struct Gpu
    {
        AdapterKind kind = AdapterKind::Hardware;
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        ComPtr<ID3D12Device> device;
        std::filesystem::path d3d12Core;
        std::filesystem::path d3d10Warp;
        std::filesystem::path sdkLayers; // with validation only
        D3D_FEATURE_LEVEL highestFeatureLevel = D3D_FEATURE_LEVEL_12_0;
        D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;
        D3D12_RESOURCE_BINDING_TIER bindingTier = D3D12_RESOURCE_BINDING_TIER_1;
        UINT waveLaneCountMin = 0;
        UINT waveLaneCountMax = 0;
    };

    ComPtr<IDXGIAdapter1> pickAdapter(IDXGIFactory6* factory, AdapterKind kind)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (kind == AdapterKind::Warp)
        {
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
            return adapter;
        }
        for (UINT i = 0;; ++i)
        {
            HRESULT hr = factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            if (hr == DXGI_ERROR_NOT_FOUND)
                throw std::runtime_error("no hardware adapter: every adapter is a software one");
            check(hr, "EnumAdapterByGpuPreference");
            DXGI_ADAPTER_DESC1 desc{};
            check(adapter->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                return adapter;
        }
    }

    D3D_SHADER_MODEL highestShaderModel(ID3D12Device* device)
    {
        // The query fails for a model newer than the runtime knows, so it walks down from the newest.
        const D3D_SHADER_MODEL models[] = {
            D3D_SHADER_MODEL_6_10, D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
            D3D_SHADER_MODEL_6_4,  D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0,
        };
        for (D3D_SHADER_MODEL model : models)
        {
            D3D12_FEATURE_DATA_SHADER_MODEL data{ model };
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data))))
                return data.HighestShaderModel;
        }
        return D3D_SHADER_MODEL_5_1;
    }

    // The device of the spike: the chosen adapter at feature level 12_0 on the app-local Agility runtime
    // (and app-local WARP), with shader model 6.6 and resource binding tier 3. `validation` enables the
    // debug layer and GPU-based validation, which must happen before the device exists.
    Gpu createDevice(AdapterKind kind, bool validation)
    {
        Gpu gpu;
        gpu.kind = kind;
        if (validation)
        {
            ComPtr<ID3D12Debug1> debug;
            check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12GetDebugInterface(ID3D12Debug1)");
            debug->EnableDebugLayer();
            debug->SetEnableGPUBasedValidation(TRUE);
        }
        ComPtr<IDXGIFactory6> factory;
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        gpu.adapter = pickAdapter(factory.Get(), kind);
        check(gpu.adapter->GetDesc1(&gpu.desc), "IDXGIAdapter1::GetDesc1");
        check(D3D12CreateDevice(gpu.adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gpu.device)), "D3D12CreateDevice");

        std::filesystem::path directory = executableDirectory();
        gpu.d3d12Core = requireLoadedFrom(L"D3D12Core.dll", directory / L"D3D12");
        if (kind == AdapterKind::Warp)
            gpu.d3d10Warp = requireLoadedFrom(L"d3d10warp.dll", directory);
        if (validation)
            gpu.sdkLayers = requireLoadedFrom(L"d3d12SDKLayers.dll", directory / L"D3D12");

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_2,
        };
        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels{};
        featureLevels.NumFeatureLevels = static_cast<UINT>(std::size(levels));
        featureLevels.pFeatureLevelsRequested = levels;
        check(gpu.device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels)), "D3D12_FEATURE_FEATURE_LEVELS");
        gpu.highestFeatureLevel = featureLevels.MaxSupportedFeatureLevel;

        gpu.shaderModel = highestShaderModel(gpu.device.Get());
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        check(gpu.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)), "D3D12_FEATURE_D3D12_OPTIONS");
        gpu.bindingTier = options.ResourceBindingTier;
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
        check(gpu.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)), "D3D12_FEATURE_D3D12_OPTIONS1");
        gpu.waveLaneCountMin = options1.WaveLaneCountMin;
        gpu.waveLaneCountMax = options1.WaveLaneCountMax;

        if (gpu.shaderModel < D3D_SHADER_MODEL_6_6)
            throw std::runtime_error("the device's highest shader model is below 6.6");
        if (gpu.bindingTier < D3D12_RESOURCE_BINDING_TIER_3)
            throw std::runtime_error("the device's resource binding tier is below 3");
        return gpu;
    }

    std::string featureLevelName(D3D_FEATURE_LEVEL level)
    {
        switch (level)
        {
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_2: return "12_2";
        default: return hex(static_cast<uint32_t>(level));
        }
    }

    std::string shaderModelName(D3D_SHADER_MODEL model)
    {
        unsigned value = static_cast<unsigned>(model);
        return std::to_string(value >> 4) + "." + std::to_string(value & 0xF);
    }

    // The user-mode driver version DXGI reports for the adapter, as a.b.c.d, or "unknown".
    std::string driverVersion(IDXGIAdapter1* adapter)
    {
        LARGE_INTEGER version{};
        if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
            return "unknown";
        uint64_t v = static_cast<uint64_t>(version.QuadPart);
        return std::to_string(v >> 48) + "." + std::to_string((v >> 32) & 0xFFFF) + "." + std::to_string((v >> 16) & 0xFFFF) + "." + std::to_string(v & 0xFFFF);
    }

    uint64_t timestampFrequency(ID3D12Device* device)
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        UINT64 frequency = 0;
        check(queue->GetTimestampFrequency(&frequency), "GetTimestampFrequency");
        return frequency;
    }

    // The adapter and runtime fields every JSON output starts with, each line ending in a comma.
    void writeAdapterFields(std::ostream& out, const Gpu& gpu)
    {
        out << "  \"adapter\": " << jsonString(narrow(gpu.desc.Description)) << ",\n";
        out << "  \"vendorId\": " << jsonString(hex(gpu.desc.VendorId)) << ",\n";
        out << "  \"deviceId\": " << jsonString(hex(gpu.desc.DeviceId)) << ",\n";
        out << "  \"driverVersion\": " << jsonString(driverVersion(gpu.adapter.Get())) << ",\n";
        out << "  \"isWarp\": " << ((gpu.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "true" : "false") << ",\n";
        out << "  \"agilitySdkVersion\": " << D3D12SDKVersion << ",\n";
        out << "  \"d3d12Core\": " << jsonString(narrow(gpu.d3d12Core.wstring())) << ",\n";
        out << "  \"d3d10Warp\": " << (gpu.d3d10Warp.empty() ? std::string("null") : jsonString(narrow(gpu.d3d10Warp.wstring()))) << ",\n";
        out << "  \"d3d12SdkLayers\": " << (gpu.sdkLayers.empty() ? std::string("null") : jsonString(narrow(gpu.sdkLayers.wstring()))) << ",\n";
        out << "  \"highestFeatureLevel\": " << jsonString(featureLevelName(gpu.highestFeatureLevel)) << ",\n";
        out << "  \"shaderModel\": " << jsonString(shaderModelName(gpu.shaderModel)) << ",\n";
        out << "  \"resourceBindingTier\": " << static_cast<int>(gpu.bindingTier) << ",\n";
        out << "  \"waveLaneCountMin\": " << gpu.waveLaneCountMin << ",\n";
        out << "  \"waveLaneCountMax\": " << gpu.waveLaneCountMax << ",\n";
    }

    int limits(AdapterKind kind)
    {
        Gpu gpu = createDevice(kind, false);
        std::ostringstream out;
        out << "{\n";
        writeAdapterFields(out, gpu);
        out << "  \"timestampFrequency\": " << timestampFrequency(gpu.device.Get()) << "\n";
        out << "}\n";
        std::cout << out.str();
        return 0;
    }

    void writeFile(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
        file.close();
        if (!file)
            throw std::runtime_error("writing " + narrow(path.wstring()) + " failed");
    }

    std::vector<uint8_t> readFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("cannot open " + narrow(path.wstring()));
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // ---- Debug layer messages ----------------------------------------------------------------------

    const char* severityName(D3D12_MESSAGE_SEVERITY severity)
    {
        switch (severity)
        {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "corruption";
        case D3D12_MESSAGE_SEVERITY_ERROR: return "error";
        case D3D12_MESSAGE_SEVERITY_WARNING: return "warning";
        case D3D12_MESSAGE_SEVERITY_INFO: return "info";
        default: return "message";
        }
    }

    struct Message
    {
        D3D12_MESSAGE_SEVERITY severity = D3D12_MESSAGE_SEVERITY_MESSAGE;
        D3D12_MESSAGE_ID id = D3D12_MESSAGE_ID_UNKNOWN;
        std::string text;
    };

    // Every message `run` saw, counted by severity; those of severity warning or worse are also kept
    // and printed on standard error. It is a process-wide object because the callback may fire while
    // the device is torn down, after the function that registered it has returned.
    struct MessageLog
    {
        std::mutex lock;
        std::array<uint64_t, 5> counts{}; // by D3D12_MESSAGE_SEVERITY: corruption, error, warning, info, message
        std::vector<Message> reported;
    };

    MessageLog& messageLog()
    {
        static MessageLog log;
        return log;
    }

    void recordMessage(D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, const char* text)
    {
        MessageLog& log = messageLog();
        std::lock_guard<std::mutex> guard(log.lock);
        size_t index = static_cast<size_t>(severity) < log.counts.size() ? static_cast<size_t>(severity) : log.counts.size() - 1;
        log.counts[index] += 1;
        if (severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        {
            Message message{ severity, id, text ? text : "" };
            std::fprintf(stderr, "d3d12 %s #%d: %s\n", severityName(severity), static_cast<int>(id), message.text.c_str());
            std::fflush(stderr);
            log.reported.push_back(std::move(message));
        }
    }

    void __stdcall onMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void*)
    {
        recordMessage(severity, id, description);
    }

    // The messages stored in the info queue, oldest first; the queue is cleared afterwards.
    std::vector<Message> takeStoredMessages(ID3D12InfoQueue* queue)
    {
        std::vector<Message> messages;
        UINT64 count = queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T length = 0;
            check(queue->GetMessage(i, nullptr, &length), "ID3D12InfoQueue::GetMessage (length)");
            std::vector<uint64_t> storage((length + sizeof(uint64_t) - 1) / sizeof(uint64_t));
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            check(queue->GetMessage(i, message, &length), "ID3D12InfoQueue::GetMessage");
            std::string text(message->pDescription, message->DescriptionByteLength > 0 ? message->DescriptionByteLength - 1 : 0);
            messages.push_back({ message->Severity, message->ID, std::move(text) });
        }
        queue->ClearStoredMessages();
        return messages;
    }

    // Watches the debug layer of `run`: an ID3D12InfoQueue1 callback where the runtime offers one,
    // else the stored messages read after every fence wait. Nothing is filtered either way.
    class MessageWatch
    {
      public:
        explicit MessageWatch(ID3D12Device* device)
        {
            check(device->QueryInterface(IID_PPV_ARGS(&mQueue)), "QueryInterface(ID3D12InfoQueue)");
            check(mQueue->PushEmptyStorageFilter(), "ID3D12InfoQueue::PushEmptyStorageFilter");
            check(mQueue->PushEmptyRetrievalFilter(), "ID3D12InfoQueue::PushEmptyRetrievalFilter");
            // Messages from creating the device were stored before any callback existed.
            drainStored();
            ComPtr<ID3D12InfoQueue1> queue1;
            DWORD cookie = 0;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue1))) &&
                SUCCEEDED(queue1->RegisterMessageCallback(onMessage, D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, nullptr, &cookie)))
                mQueue1 = queue1;
        }

        bool usesCallback() const
        {
            return mQueue1 != nullptr;
        }

        // Called after every fence wait: reads the stored messages when there is no callback.
        void poll()
        {
            if (!mQueue1)
                drainStored();
        }

      private:
        void drainStored()
        {
            for (const Message& message : takeStoredMessages(mQueue.Get()))
                recordMessage(message.severity, message.id, message.text.c_str());
        }

        ComPtr<ID3D12InfoQueue> mQueue;
        ComPtr<ID3D12InfoQueue1> mQueue1; // kept registered until the process ends
    };

    // ---- Resources ---------------------------------------------------------------------------------

    constexpr uint32_t kSlices = 64;
    constexpr uint32_t kSliceWords = 1u << 18; // 1 MiB of 32-bit words
    constexpr uint64_t kSliceBytes = uint64_t(kSliceWords) * 4u;
    constexpr uint64_t kDataBytes = kSliceBytes * kSlices;
    constexpr uint32_t kRootBlockBytes = kSlices * 8u; // 64 Addrs
    constexpr UINT kHeapSlots = 5 + 2 * kSlices;       // 133
    constexpr UINT kOutputSrvSlot = 1;
    constexpr UINT kTableSrvSlot = 3;
    constexpr UINT kDataSrvSlot0 = 5;

    // The hash every data word holds, the G1 hash of Shaders/Bench/BenchLoads.slang.
    constexpr uint32_t mix(uint32_t value)
    {
        const uint32_t state = value * 747796405u + 2891336453u;
        const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        return (word >> 22u) ^ word;
    }

    ComPtr<ID3D12Resource> createBuffer(
        ID3D12Device* device, D3D12_HEAP_TYPE heapType, uint64_t bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name
    )
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = heapType;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        ComPtr<ID3D12Resource> buffer;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer)), "CreateCommittedResource");
        check(buffer->SetName(name), "ID3D12Resource::SetName");
        return buffer;
    }

    // Raw views over 32-bit elements, the backend's view pair: R32_TYPELESS with the RAW flag.
    D3D12_SHADER_RESOURCE_VIEW_DESC rawSrvDesc(uint64_t firstElement, uint32_t numElements)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.FirstElement = firstElement;
        desc.Buffer.NumElements = numElements;
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        return desc;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC rawUavDesc(uint64_t firstElement, uint32_t numElements)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.FirstElement = firstElement;
        desc.Buffer.NumElements = numElements;
        desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        return desc;
    }

    D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

    // The backend's global root signature: version 1.1, both heaps directly indexed, the root SRV at
    // t0 (DATA_VOLATILE) and one 32-bit root constant at b1.
    ComPtr<ID3D12RootSignature> createRootSignature(ID3D12Device* device)
    {
        D3D12_ROOT_PARAMETER1 parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].Descriptor.RegisterSpace = 0;
        parameters[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants.ShaderRegister = 1;
        parameters[1].Constants.RegisterSpace = 0;
        parameters[1].Constants.Num32BitValues = 1;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = static_cast<UINT>(std::size(parameters));
        desc.Desc_1_1.pParameters = parameters;
        desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> error;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
        if (FAILED(hr))
        {
            std::string text = error ? std::string(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize()) : "";
            throw std::runtime_error("D3D12SerializeVersionedRootSignature failed with HRESULT " + hex(static_cast<uint32_t>(hr)) + ": " + text);
        }
        ComPtr<ID3D12RootSignature> signature;
        check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature)), "CreateRootSignature");
        return signature;
    }

    // One direct queue, one command list and a fence; every submission is waited on.
    class Submitter
    {
      public:
        explicit Submitter(ID3D12Device* device) : mDevice(device)
        {
            D3D12_COMMAND_QUEUE_DESC desc{};
            desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&mQueue)), "CreateCommandQueue");
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mAllocator)), "CreateCommandAllocator");
            check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mAllocator.Get(), nullptr, IID_PPV_ARGS(&mList)), "CreateCommandList");
            check(mList->Close(), "ID3D12GraphicsCommandList::Close");
            check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)), "CreateFence");
            mEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!mEvent)
                throw std::runtime_error("CreateEventW failed");
        }

        ~Submitter()
        {
            CloseHandle(mEvent);
        }

        Submitter(const Submitter&) = delete;
        Submitter& operator=(const Submitter&) = delete;

        ID3D12CommandQueue* queue() const
        {
            return mQueue.Get();
        }

        ID3D12GraphicsCommandList* begin()
        {
            check(mAllocator->Reset(), "ID3D12CommandAllocator::Reset");
            check(mList->Reset(mAllocator.Get(), nullptr), "ID3D12GraphicsCommandList::Reset");
            return mList.Get();
        }

        void submitAndWait()
        {
            check(mList->Close(), "ID3D12GraphicsCommandList::Close");
            ID3D12CommandList* lists[] = { mList.Get() };
            mQueue->ExecuteCommandLists(1, lists);
            check(mQueue->Signal(mFence.Get(), ++mValue), "ID3D12CommandQueue::Signal");
            if (mFence->GetCompletedValue() < mValue)
            {
                check(mFence->SetEventOnCompletion(mValue, mEvent), "ID3D12Fence::SetEventOnCompletion");
                if (WaitForSingleObject(mEvent, INFINITE) != WAIT_OBJECT_0)
                    throw std::runtime_error("WaitForSingleObject on the fence event failed");
            }
            check(mDevice->GetDeviceRemovedReason(), "the device was removed; GetDeviceRemovedReason");
        }

      private:
        ID3D12Device* mDevice;
        ComPtr<ID3D12CommandQueue> mQueue;
        ComPtr<ID3D12CommandAllocator> mAllocator;
        ComPtr<ID3D12GraphicsCommandList> mList;
        ComPtr<ID3D12Fence> mFence;
        HANDLE mEvent = nullptr;
        uint64_t mValue = 0;
    };

    // ---- Kernels and the CPU reference ------------------------------------------------------------

    enum class Arm
    {
        F,
        H1,
        H2,
        H1m
    };

    struct Kernel
    {
        Arm arm;
        const char* armName;
        uint32_t mode; // 0 uniform, 1 wave-uniform, 2 divergent
    };

    // The forward order: F, H1, H2, H1m, each uniform, wave-uniform and divergent.
    constexpr Kernel kKernels[] = {
        { Arm::F, "F", 0 },   { Arm::F, "F", 1 },   { Arm::F, "F", 2 },   { Arm::H1, "H1", 0 },   { Arm::H1, "H1", 1 },   { Arm::H1, "H1", 2 },
        { Arm::H2, "H2", 0 }, { Arm::H2, "H2", 1 }, { Arm::H2, "H2", 2 }, { Arm::H1m, "H1m", 0 }, { Arm::H1m, "H1m", 1 }, { Arm::H1m, "H1m", 2 },
    };

    constexpr const char* kModeLetters[] = { "u", "w", "d" };
    constexpr const char* kModeNames[] = { "uniform", "wave-uniform", "divergent" };

    // The slice thread t reads: 0, one per 64 consecutive threads, or one per thread.
    uint32_t sliceOf(uint32_t t, uint32_t mode)
    {
        if (mode == 0)
            return 0;
        if (mode == 1)
            return (t >> 6) & 63u;
        return t & 63u;
    }

    // What thread t stores, from the protocol: for j in 0..15 the uint4 at 16-byte element
    // e = (j << 12) | (t & 4095) of slice k, after 16 LCG rounds per component in the alu variant,
    // summed with wrapping 32-bit arithmetic. `words` is the data as the CPU wrote it, mix(w) at w.
    std::vector<uint32_t> reference(const std::vector<uint32_t>& words, uint32_t threads, uint32_t mode, bool alu)
    {
        std::vector<uint32_t> out(size_t(threads) * 4u);
        for (uint32_t t = 0; t < threads; ++t)
        {
            uint32_t k = sliceOf(t, mode);
            uint32_t sum[4] = {};
            for (uint32_t j = 0; j < 16; ++j)
            {
                uint32_t e = (j << 12) | (t & 4095u);
                size_t w = size_t(k) * kSliceWords + size_t(e) * 4u;
                for (uint32_t c = 0; c < 4; ++c)
                {
                    uint32_t v = words[w + c];
                    if (alu)
                    {
                        for (uint32_t r = 0; r < 16; ++r)
                            v = v * 1664525u + 1013904223u;
                    }
                    sum[c] += v;
                }
            }
            for (uint32_t c = 0; c < 4; ++c)
                out[size_t(t) * 4u + c] = sum[c];
        }
        return out;
    }

    // ---- Arguments ----------------------------------------------------------------------------------

    // The `--name value` options and bare `--flag`s after the command; anything else is an error.
    struct Arguments
    {
        std::map<std::string, std::string> values;
        std::set<std::string> flags;

        bool has(const std::string& name) const
        {
            return values.count(name) != 0;
        }

        const std::string& get(const std::string& name) const
        {
            auto found = values.find(name);
            if (found == values.end())
                throw std::runtime_error("missing --" + name);
            return found->second;
        }
    };

    Arguments parseArguments(int argc, char** argv, const std::set<std::string>& valueNames, const std::set<std::string>& flagNames)
    {
        Arguments arguments;
        for (int i = 2; i < argc; ++i)
        {
            std::string word = argv[i];
            std::string name = word.rfind("--", 0) == 0 ? word.substr(2) : "";
            if (flagNames.count(name))
                arguments.flags.insert(name);
            else if (valueNames.count(name) && i + 1 < argc)
                arguments.values[name] = argv[++i];
            else
                throw std::runtime_error("unexpected argument " + word);
        }
        return arguments;
    }

    AdapterKind adapterKind(const Arguments& arguments)
    {
        if (!arguments.has("adapter"))
            return AdapterKind::Hardware;
        const std::string& value = arguments.get("adapter");
        if (value == "hardware")
            return AdapterKind::Hardware;
        if (value == "warp")
            return AdapterKind::Warp;
        throw std::runtime_error("--adapter takes hardware or warp, not " + value);
    }

    uint32_t parseUnsigned(const std::string& text, const std::string& what)
    {
        size_t used = 0;
        unsigned long long value = 0;
        try
        {
            value = std::stoull(text, &used, 10);
        }
        catch (const std::exception&)
        {
            used = 0;
        }
        if (text.empty() || used != text.size() || value > UINT32_MAX)
            throw std::runtime_error(what + " takes an unsigned 32-bit integer, not " + text);
        return static_cast<uint32_t>(value);
    }

    // ---- run -----------------------------------------------------------------------------------------

    struct KernelResult
    {
        std::string name;
        const char* arm;
        const char* baseMode;
        uint64_t mismatches = 0;
        int64_t firstBad = -1;
        std::vector<double> gpuNs; // timing mode: the 200 kept durations
        double gpuMedianNs = 0;
    };

    // Timing mode: dispatches per command list, the first ones dropped, and so the ones kept.
    constexpr uint32_t kTimedDispatches = 250;
    constexpr uint32_t kWarmupDispatches = 50;

    bool onAcPower()
    {
        SYSTEM_POWER_STATUS status{};
        if (!GetSystemPowerStatus(&status))
            throw std::runtime_error("GetSystemPowerStatus failed");
        return status.ACLineStatus == 1;
    }

    double median(std::vector<double> values)
    {
        std::sort(values.begin(), values.end());
        size_t n = values.size();
        return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
    }

    int run(const Arguments& arguments)
    {
        const AdapterKind kind = adapterKind(arguments);
        const std::filesystem::path kernelDirectory = arguments.get("kernels");
        const uint32_t threads = parseUnsigned(arguments.get("threads"), "--threads");
        const std::string variant = arguments.get("variant");
        const uint32_t order = parseUnsigned(arguments.get("order"), "--order");
        const std::string mode = arguments.get("mode");
        const std::filesystem::path outPath = arguments.get("out");
        const bool corruptData = arguments.flags.count("corrupt-data") != 0;
        if (threads == 0 || threads % 64 != 0 || threads / 64 > uint32_t(D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION))
            throw std::runtime_error("--threads must be a positive multiple of 64 within one dispatch dimension");
        if (variant != "fetch" && variant != "alu")
            throw std::runtime_error("--variant takes fetch or alu, not " + variant);
        if (mode != "correctness" && mode != "timing")
            throw std::runtime_error("--mode takes correctness or timing, not " + mode);
        const bool timing = mode == "timing";
        if (timing && corruptData)
            throw std::runtime_error("--corrupt-data belongs to --mode correctness");
        const bool powerOnAcStart = onAcPower();
        const bool alu = variant == "alu";
        const uint64_t outputBytes = uint64_t(threads) * 16u;

        Gpu gpu = createDevice(kind, !timing);
        ID3D12Device* device = gpu.device.Get();
        std::optional<MessageWatch> watch;
        if (!timing)
            watch.emplace(device);
        auto poll = [&]() {
            if (watch)
                watch->poll();
        };
        Submitter submitter(device);
        ComPtr<ID3D12RootSignature> rootSignature = createRootSignature(device);

        // One compute pipeline per kernel of the variant, from its DXIL, with the global root signature.
        std::vector<ComPtr<ID3D12PipelineState>> pipelines;
        for (const Kernel& kernel : kKernels)
        {
            std::vector<uint8_t> dxil = readFile(kernelDirectory / (std::string(kernel.armName) + "-" + kModeLetters[kernel.mode] + "-" + variant + ".dxil"));
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = rootSignature.Get();
            desc.CS = { dxil.data(), dxil.size() };
            ComPtr<ID3D12PipelineState> pipeline;
            check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)), std::string("CreateComputePipelineState ") + kernel.armName);
            pipelines.push_back(pipeline);
        }

        // Buffers, all in DEFAULT heaps except the upload, zero and readback staging.
        const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> data = createBuffer(device, D3D12_HEAP_TYPE_DEFAULT, kDataBytes, uav, D3D12_RESOURCE_STATE_COMMON, L"spike.data");
        ComPtr<ID3D12Resource> rootH1 =
            createBuffer(device, D3D12_HEAP_TYPE_DEFAULT, kRootBlockBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, L"spike.rootH1");
        ComPtr<ID3D12Resource> rootH2 =
            createBuffer(device, D3D12_HEAP_TYPE_DEFAULT, 8, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, L"spike.rootH2");
        ComPtr<ID3D12Resource> table = createBuffer(device, D3D12_HEAP_TYPE_DEFAULT, kRootBlockBytes, uav, D3D12_RESOURCE_STATE_COMMON, L"spike.tableA");
        ComPtr<ID3D12Resource> output = createBuffer(device, D3D12_HEAP_TYPE_DEFAULT, outputBytes, uav, D3D12_RESOURCE_STATE_COMMON, L"spike.output");
        const uint64_t uploadRootH1 = kDataBytes;
        const uint64_t uploadRootH2 = uploadRootH1 + kRootBlockBytes;
        const uint64_t uploadTable = uploadRootH2 + 8;
        const uint64_t uploadBytes = uploadTable + kRootBlockBytes;
        ComPtr<ID3D12Resource> upload =
            createBuffer(device, D3D12_HEAP_TYPE_UPLOAD, uploadBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, L"spike.upload");
        ComPtr<ID3D12Resource> zeros =
            createBuffer(device, D3D12_HEAP_TYPE_UPLOAD, outputBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, L"spike.zeros");
        ComPtr<ID3D12Resource> readback =
            createBuffer(device, D3D12_HEAP_TYPE_READBACK, outputBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, L"spike.readback");

        // The data as the CPU writes it, word w = mix(w), and the three references computed from it.
        std::vector<uint32_t> words(kDataBytes / 4u);
        for (size_t w = 0; w < words.size(); ++w)
            words[w] = mix(static_cast<uint32_t>(w));
        std::vector<uint32_t> expected[3];
        for (uint32_t m = 0; m < 3; ++m)
            expected[m] = reference(words, threads, m, alu);

        // The root block of H1 and H1m and table A: entry k is Addr (5 + 2k, 0), data view k.
        // H2's root block is Addr A, (3, 0), table A's view.
        uint32_t addrs[kSlices * 2];
        for (uint32_t k = 0; k < kSlices; ++k)
        {
            addrs[2 * k] = kDataSrvSlot0 + 2 * k;
            addrs[2 * k + 1] = 0;
        }
        const uint32_t addrA[2] = { kTableSrvSlot, 0 };
        {
            D3D12_RANGE none{ 0, 0 };
            void* mappedUpload = nullptr;
            check(upload->Map(0, &none, &mappedUpload), "Map(upload)");
            auto* mapped = static_cast<uint8_t*>(mappedUpload);
            std::memcpy(mapped, words.data(), kDataBytes);
            if (corruptData)
            {
                // In the GPU copy only (the references keep mix(w)), word 0 changes in elements 0 to
                // 63 of every slice, which thread k of a divergent kernel reads in slice k and
                // threads 0 to 63 of the others read in slice 0, and in element 64k of slice k,
                // which the first thread of group k of a wave-uniform kernel reads. So every kernel
                // mismatches, in every slice its threads reach.
                auto* gpuWords = reinterpret_cast<uint32_t*>(mapped);
                for (uint32_t k = 0; k < kSlices; ++k)
                {
                    const size_t slice = size_t(k) * kSliceWords;
                    for (uint32_t e = 0; e < 64; ++e)
                        gpuWords[slice + size_t(e) * 4u] = ~words[slice + size_t(e) * 4u];
                    gpuWords[slice + size_t(64u * k) * 4u] = ~words[slice + size_t(64u * k) * 4u];
                }
            }
            std::memcpy(mapped + uploadRootH1, addrs, sizeof(addrs));
            std::memcpy(mapped + uploadRootH2, addrA, sizeof(addrA));
            std::memcpy(mapped + uploadTable, addrs, sizeof(addrs));
            upload->Unmap(0, nullptr);
            void* mappedZeros = nullptr;
            check(zeros->Map(0, &none, &mappedZeros), "Map(zeros)");
            std::memset(mappedZeros, 0, static_cast<size_t>(outputBytes));
            zeros->Unmap(0, nullptr);
        }

        // One copy fills every DEFAULT buffer; implicit promotion takes them to COPY_DEST and they
        // decay to COMMON when it completes.
        {
            ID3D12GraphicsCommandList* list = submitter.begin();
            list->CopyBufferRegion(data.Get(), 0, upload.Get(), 0, kDataBytes);
            list->CopyBufferRegion(rootH1.Get(), 0, upload.Get(), uploadRootH1, kRootBlockBytes);
            list->CopyBufferRegion(rootH2.Get(), 0, upload.Get(), uploadRootH2, 8);
            list->CopyBufferRegion(table.Get(), 0, upload.Get(), uploadTable, kRootBlockBytes);
            submitter.submitAndWait();
            poll();
        }

        // The heap: slot 0 a null SRV, 1 and 2 the output, 3 and 4 table A, 5 + 2k and 6 + 2k data view k.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kHeapSlots;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;
        check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
        const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto slot = [&](UINT index) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += SIZE_T(index) * increment;
            return handle;
        };
        // The root signature indexes the sampler heap directly too, so the layer requires one bound
        // before it; no kernel reads it, and its one slot holds a default point sampler.
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
        samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDesc.NumDescriptors = 1;
        samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> samplerHeap;
        check(device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&samplerHeap)), "CreateDescriptorHeap(sampler)");
        D3D12_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        device->CreateSampler(&sampler, samplerHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = rawSrvDesc(0, 1);
        device->CreateShaderResourceView(nullptr, &nullDesc, slot(0));
        auto pair = [&](ID3D12Resource* resource, UINT srvSlot, uint64_t firstElement, uint32_t numElements) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = rawSrvDesc(firstElement, numElements);
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = rawUavDesc(firstElement, numElements);
            device->CreateShaderResourceView(resource, &srv, slot(srvSlot));
            device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, slot(srvSlot + 1));
        };
        pair(output.Get(), kOutputSrvSlot, 0, static_cast<uint32_t>(outputBytes / 4u));
        pair(table.Get(), kTableSrvSlot, 0, kRootBlockBytes / 4u);
        for (uint32_t k = 0; k < kSlices; ++k)
            pair(data.Get(), kDataSrvSlot0 + 2 * k, uint64_t(k) * kSliceWords, kSliceWords);

        // Timing mode: a timestamp heap of two queries per dispatch and a readback for its resolve.
        const uint32_t queryCount = 2 * kTimedDispatches;
        ComPtr<ID3D12QueryHeap> queryHeap;
        ComPtr<ID3D12Resource> queryReadback;
        UINT64 timestampFrequency = 0;
        if (timing)
        {
            D3D12_QUERY_HEAP_DESC queryDesc{};
            queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            queryDesc.Count = queryCount;
            check(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queryHeap)), "CreateQueryHeap");
            queryReadback = createBuffer(
                device,
                D3D12_HEAP_TYPE_READBACK,
                uint64_t(queryCount) * sizeof(UINT64),
                D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST,
                L"spike.queries"
            );
            check(submitter.queue()->GetTimestampFrequency(&timestampFrequency), "GetTimestampFrequency");
        }

        // Forward order in even processes, reversed in odd ones; each kernel is one dispatch on a
        // zeroed output, so a kernel that writes nothing fails.
        std::vector<KernelResult> results;
        const size_t kernelCount = std::size(kKernels);
        bool failed = false;
        for (size_t step = 0; step < kernelCount; ++step)
        {
            const size_t index = order % 2 == 0 ? step : kernelCount - 1 - step;
            const Kernel& kernel = kKernels[index];
            D3D12_GPU_VIRTUAL_ADDRESS root = kernel.arm == Arm::F    ? data->GetGPUVirtualAddress()
                                             : kernel.arm == Arm::H2 ? rootH2->GetGPUVirtualAddress()
                                                                     : rootH1->GetGPUVirtualAddress();
            ID3D12GraphicsCommandList* list = submitter.begin();
            list->CopyBufferRegion(output.Get(), 0, zeros.Get(), 0, outputBytes);
            D3D12_RESOURCE_BARRIER toUav = transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &toUav);
            ID3D12DescriptorHeap* heaps[] = { heap.Get(), samplerHeap.Get() };
            list->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);
            list->SetComputeRootSignature(rootSignature.Get());
            list->SetPipelineState(pipelines[index].Get());
            list->SetComputeRootShaderResourceView(0, root);
            list->SetComputeRoot32BitConstant(1, 0, 0);
            if (timing)
            {
                // Each dispatch sits between two timestamps; the UAV barrier that orders consecutive
                // dispatches falls after the second one, outside the timed span.
                D3D12_RESOURCE_BARRIER uavBarrier{};
                uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uavBarrier.UAV.pResource = output.Get();
                for (uint32_t i = 0; i < kTimedDispatches; ++i)
                {
                    list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * i);
                    list->Dispatch(threads / 64, 1, 1);
                    list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * i + 1);
                    list->ResourceBarrier(1, &uavBarrier);
                }
            }
            else
            {
                list->Dispatch(threads / 64, 1, 1);
            }
            D3D12_RESOURCE_BARRIER toCopy = transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &toCopy);
            list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, outputBytes);
            if (timing)
                list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, queryCount, queryReadback.Get(), 0);
            submitter.submitAndWait();
            poll();

            KernelResult result;
            result.name = std::string(kernel.armName) + "-" + kModeLetters[kernel.mode];
            result.arm = kernel.armName;
            result.baseMode = kModeNames[kernel.mode];
            D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(outputBytes) };
            void* mappedReadback = nullptr;
            check(readback->Map(0, &readRange, &mappedReadback), "Map(readback)");
            const uint32_t* got = static_cast<const uint32_t*>(mappedReadback);
            const std::vector<uint32_t>& want = expected[kernel.mode];
            for (uint32_t t = 0; t < threads; ++t)
            {
                if (std::memcmp(got + size_t(t) * 4u, want.data() + size_t(t) * 4u, 16) != 0)
                {
                    if (result.mismatches == 0)
                        result.firstBad = t;
                    result.mismatches += 1;
                }
            }
            D3D12_RANGE written{ 0, 0 };
            readback->Unmap(0, &written);
            if (timing)
            {
                D3D12_RANGE queryRange{ 0, SIZE_T(queryCount) * sizeof(UINT64) };
                void* mappedQueries = nullptr;
                check(queryReadback->Map(0, &queryRange, &mappedQueries), "Map(queries)");
                const auto* ticks = static_cast<const UINT64*>(mappedQueries);
                for (uint32_t i = kWarmupDispatches; i < kTimedDispatches; ++i)
                    result.gpuNs.push_back(double(ticks[2 * i + 1] - ticks[2 * i]) * 1e9 / double(timestampFrequency));
                queryReadback->Unmap(0, &written);
                result.gpuMedianNs = median(result.gpuNs);
            }
            std::printf("kernel %s-%s: %llu mismatches\n", result.name.c_str(), variant.c_str(), static_cast<unsigned long long>(result.mismatches));
            failed = failed || result.mismatches != 0;
            results.push_back(result);
        }

        // GPU-based validation reports after the GPU finishes; one more submission and wait gives the
        // layer a chance to deliver whatever it found in the last dispatch before the counts are read.
        submitter.begin();
        submitter.submitAndWait();
        poll();

        std::array<uint64_t, 5> counts;
        std::vector<Message> reported;
        {
            MessageLog& log = messageLog();
            std::lock_guard<std::mutex> guard(log.lock);
            counts = log.counts;
            reported = log.reported;
        }
        failed = failed || counts[0] + counts[1] + counts[2] != 0;

        const bool powerOnAcEnd = onAcPower();
        std::ostringstream out;
        out << "{\n";
        writeAdapterFields(out, gpu);
        out << "  \"threads\": " << threads << ",\n";
        out << "  \"variant\": " << jsonString(variant) << ",\n";
        out << "  \"order\": " << order << ",\n";
        out << "  \"mode\": " << jsonString(mode) << ",\n";
        out << "  \"validation\": " << (timing ? "false" : "true") << ",\n";
        out << "  \"corruptData\": " << (corruptData ? "true" : "false") << ",\n";
        if (timing)
        {
            out << "  \"powerOnAcStart\": " << (powerOnAcStart ? "true" : "false") << ",\n";
            out << "  \"powerOnAcEnd\": " << (powerOnAcEnd ? "true" : "false") << ",\n";
            out << "  \"timestampFrequency\": " << timestampFrequency << ",\n";
            out << "  \"messageSource\": \"none: debug layer off\",\n";
        }
        else
        {
            out << "  \"messageSource\": " << jsonString(watch->usesCallback() ? "ID3D12InfoQueue1 callback" : "ID3D12InfoQueue polling") << ",\n";
        }
        out << "  \"kernels\": [\n";
        for (size_t i = 0; i < results.size(); ++i)
        {
            const KernelResult& r = results[i];
            out << "    {\"name\": " << jsonString(r.name) << ", \"arm\": " << jsonString(r.arm) << ", \"baseMode\": " << jsonString(r.baseMode)
                << ", \"mismatches\": " << r.mismatches << ", \"firstBad\": " << r.firstBad;
            if (timing)
            {
                out << ", \"notRun\": false, \"gpuMedianNs\": " << std::fixed << std::setprecision(3) << r.gpuMedianNs << ", \"gpuNs\": [";
                for (size_t j = 0; j < r.gpuNs.size(); ++j)
                    out << (j ? ", " : "") << r.gpuNs[j];
                out << "]";
            }
            out << "}" << (i + 1 < results.size() ? "," : "") << "\n";
        }
        out << "  ],\n";
        out << "  \"messages\": {\"corruption\": " << counts[0] << ", \"error\": " << counts[1] << ", \"warning\": " << counts[2] << ", \"info\": " << counts[3]
            << ", \"message\": " << counts[4] << "},\n";
        out << "  \"reported\": [\n";
        for (size_t i = 0; i < reported.size(); ++i)
        {
            const Message& m = reported[i];
            out << "    {\"severity\": " << jsonString(severityName(m.severity)) << ", \"id\": " << static_cast<int>(m.id)
                << ", \"text\": " << jsonString(m.text) << "}" << (i + 1 < reported.size() ? "," : "") << "\n";
        }
        out << "  ],\n";
        out << "  \"passed\": " << (failed ? "false" : "true") << "\n";
        out << "}\n";
        writeFile(outPath, out.str());
        return failed ? 1 : 0;
    }

    // ---- viewcap -------------------------------------------------------------------------------------

    // One raw SRV of 2^27 + 1 32-bit elements over a buffer of 2^29 + 4 bytes, under the debug layer:
    // records what the layer said about it and whether the device survived. The messages go to the
    // JSON only.
    int viewcap(const Arguments& arguments)
    {
        const AdapterKind kind = adapterKind(arguments);
        const std::filesystem::path outPath = arguments.get("out");
        constexpr uint32_t numElements = (1u << 27) + 1u;
        constexpr uint64_t bufferBytes = (uint64_t(1) << 29) + 4u;

        Gpu gpu = createDevice(kind, true);
        ID3D12Device* device = gpu.device.Get();
        ComPtr<ID3D12InfoQueue> queue;
        check(device->QueryInterface(IID_PPV_ARGS(&queue)), "QueryInterface(ID3D12InfoQueue)");
        check(queue->PushEmptyStorageFilter(), "ID3D12InfoQueue::PushEmptyStorageFilter");
        check(queue->PushEmptyRetrievalFilter(), "ID3D12InfoQueue::PushEmptyRetrievalFilter");
        std::vector<Message> setup = takeStoredMessages(queue.Get());

        ComPtr<ID3D12Resource> buffer =
            createBuffer(device, D3D12_HEAP_TYPE_DEFAULT, bufferBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, L"spike.viewcap");
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> heap;
        check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
        for (Message& message : takeStoredMessages(queue.Get()))
            setup.push_back(std::move(message));

        D3D12_SHADER_RESOURCE_VIEW_DESC desc = rawSrvDesc(0, numElements);
        device->CreateShaderResourceView(buffer.Get(), &desc, heap->GetCPUDescriptorHandleForHeapStart());
        std::vector<Message> probe = takeStoredMessages(queue.Get());
        HRESULT removed = device->GetDeviceRemovedReason();

        auto writeMessages = [](std::ostream& out, const std::vector<Message>& messages) {
            out << "[";
            for (size_t i = 0; i < messages.size(); ++i)
            {
                const Message& m = messages[i];
                out << (i ? ", " : "") << "{\"severity\": " << jsonString(severityName(m.severity)) << ", \"id\": " << static_cast<int>(m.id)
                    << ", \"text\": " << jsonString(m.text) << "}";
            }
            out << "]";
        };
        bool raised = false;
        for (const Message& m : probe)
            raised = raised || m.severity <= D3D12_MESSAGE_SEVERITY_WARNING;
        std::ostringstream out;
        out << "{\n";
        writeAdapterFields(out, gpu);
        out << "  \"bufferBytes\": " << bufferBytes << ",\n";
        out << "  \"numElements\": " << numElements << ",\n";
        out << "  \"raised\": " << (raised ? "true" : "false") << ",\n";
        out << "  \"messages\": ";
        writeMessages(out, probe);
        out << ",\n  \"setupMessages\": ";
        writeMessages(out, setup);
        out << ",\n  \"deviceRemoved\": " << (FAILED(removed) ? "true" : "false") << ",\n";
        out << "  \"deviceRemovedReason\": " << jsonString(hex(static_cast<uint32_t>(removed))) << "\n";
        out << "}\n";
        writeFile(outPath, out.str());
        return 0;
    }

} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string command = argc > 1 ? argv[1] : "";
        if (command == "limits")
            return limits(adapterKind(parseArguments(argc, argv, { "adapter" }, {})));
        if (command == "run")
            return run(parseArguments(argc, argv, { "adapter", "kernels", "threads", "variant", "order", "mode", "out" }, { "corrupt-data" }));
        if (command == "viewcap")
            return viewcap(parseArguments(argc, argv, { "adapter", "out" }, {}));
        throw std::runtime_error("usage: Harness limits|run|viewcap --adapter hardware|warp ...");
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
