//----------------------------------------------------------------------------------------------------------------------
/// @file NativeContext.cpp
/// @brief Implements the native arm's D3D12 device, root signature, heaps, frame slots, setup submissions and teardown.
//----------------------------------------------------------------------------------------------------------------------
#include "NativeD3D12/NativeContext.h"

#include "Host/ExecutableDirectory.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <system_error>
#include <tuple>
#include <utility>

namespace rojoRHI::bench {
    namespace {
        // The Agility SDK version the device factory loads, the pinned runtime's: D3D12Core.dll in D3D12\ beside the
        // executable.
        constexpr UINT kAgilitySdkVersion = 619;
        static_assert(D3D12_SDK_VERSION == kAgilitySdkVersion, "the Agility SDK headers are not version 619");

        // How long a fence wait may take, as long as RojoRHI's frame wait in the benchmark.
        constexpr DWORD kWaitMilliseconds = 60'000;

        // The most four-byte elements one raw view reaches, the first 512 MiB of a buffer, as in RojoRHI.
        constexpr uint64_t kRawViewElements = uint64_t{ 1 } << 27;

        // The adapter kind the build selects (rojorhi_d3d12_adapter).
#if defined(ROJORHI_D3D12_ADAPTER_WARP)
        constexpr bool kWarpAdapter = true;
#else
        constexpr bool kWarpAdapter = false;
#endif

        // Why the clear-value mismatches are dropped: the textures of both arms carry no optimized clear value, so every
        // clear of a render target or a depth target differs from the value it was created with.
        constexpr std::string_view kNoClearValue = "the textures carry no optimized clear value, as RojoRHI's do";
        // Why the hint on lists of barriers alone is dropped.
        constexpr std::string_view kBarrierOnly = "a list that records only barriers is valid usage";

        // A message N drops, with the reason, a reason RojoRHI's D3D12 backend drops it for too.
        struct DeniedMessage
        {
            D3D12_MESSAGE_ID id;
            std::string_view reason;
        };

        // The backend's other dropped messages are the per-object creation and destruction notices, which are information
        // messages that nativeMessage never writes, and the pipeline library's outcomes, from calls N never makes.
        constexpr std::array<DeniedMessage, 3> kDenied{ {
            { D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE, kNoClearValue },
            { D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE, kNoClearValue },
            { D3D12_MESSAGE_ID_NON_OPTIMAL_BARRIER_ONLY_EXECUTE_COMMAND_LISTS, kBarrierOnly },
        } };

        //==============================================================================================================
        uint32_t bits(HRESULT result)
        {
            return static_cast<uint32_t>(result);
        }

        //==============================================================================================================
        std::string failed(std::string_view what, HRESULT result)
        {
            return std::format("{} failed with HRESULT 0x{:08X}", what, bits(result));
        }

        //==============================================================================================================
        std::string utf8(std::wstring_view text)
        {
            if (text.empty())
            {
                return {};
            }
            const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<size_t>(std::max(length, 0)), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
            return result;
        }

        //==============================================================================================================
        std::wstring widen(std::string_view text)
        {
            if (text.empty())
            {
                return {};
            }
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring result(static_cast<size_t>(std::max(length, 0)), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
            return result;
        }

        //==============================================================================================================
        size_t alignUp(size_t value, size_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        //==============================================================================================================
        // The highest shader model of `device`. The runtime refuses a model newer than it knows with E_INVALIDARG, so the
        // request steps down until it is answered or reaches Shader Model 6.6.
        HRESULT highestShaderModel(ID3D12Device12* device, D3D12_FEATURE_DATA_SHADER_MODEL& model)
        {
            model = { D3D_HIGHEST_SHADER_MODEL };
            HRESULT result = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, static_cast<UINT>(sizeof(model)));
            while (result == E_INVALIDARG && model.HighestShaderModel > D3D_SHADER_MODEL_6_6)
            {
                model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(static_cast<int>(model.HighestShaderModel) - 1);
                result = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, static_cast<UINT>(sizeof(model)));
            }
            return result;
        }

        //==============================================================================================================
        // The hardware floor RojoRHI's D3D12 backend requires of an adapter (ADR 0010): Shader Model 6.6, resource binding
        // tier 3, typed UAV loads of the additional formats, resource heap tier 2, enhanced barriers and dynamic depth
        // bias. Returns the first item missing, or nothing.
        std::optional<std::string> missingFloor(ID3D12Device12* device)
        {
            D3D12_FEATURE_DATA_SHADER_MODEL model{};
            if (FAILED(highestShaderModel(device, model)) || model.HighestShaderModel < D3D_SHADER_MODEL_6_6)
            {
                return std::string("Shader Model 6.6");
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, static_cast<UINT>(sizeof(options)))) ||
                options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
            {
                return std::string("resource binding tier 3");
            }
            if (options.TypedUAVLoadAdditionalFormats == FALSE)
            {
                return std::string("TypedUAVLoadAdditionalFormats");
            }
            if (options.ResourceHeapTier < D3D12_RESOURCE_HEAP_TIER_2)
            {
                return std::string("resource heap tier 2");
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, static_cast<UINT>(sizeof(options12)))) ||
                options12.EnhancedBarriersSupported == FALSE)
            {
                return std::string("enhanced barriers");
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16{};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, static_cast<UINT>(sizeof(options16)))) ||
                options16.DynamicDepthBiasSupported == FALSE)
            {
                return std::string("dynamic depth bias");
            }
            return std::nullopt;
        }

        //==============================================================================================================
        // A raw view of `elements` four-byte elements, as a module Addr reads a buffer.
        D3D12_SHADER_RESOURCE_VIEW_DESC rawShaderResource(uint64_t elements)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Buffer.FirstElement = 0;
            desc.Buffer.NumElements = static_cast<UINT>(elements);
            desc.Buffer.StructureByteStride = 0;
            desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            return desc;
        }

        //==============================================================================================================
        D3D12_UNORDERED_ACCESS_VIEW_DESC rawUnorderedAccess(uint64_t elements)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.FirstElement = 0;
            desc.Buffer.NumElements = static_cast<UINT>(elements);
            desc.Buffer.StructureByteStride = 0;
            desc.Buffer.CounterOffsetInBytes = 0;
            desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            return desc;
        }

        //==============================================================================================================
        // The accesses a subresource in `layout` admits, the ones a barrier out of it names; none for an undefined layout.
        D3D12_BARRIER_ACCESS layoutAccess(D3D12_BARRIER_LAYOUT layout)
        {
            switch (layout)
            {
            case D3D12_BARRIER_LAYOUT_COMMON: return D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_COPY_SOURCE | D3D12_BARRIER_ACCESS_COPY_DEST;
            case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE: return D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
            case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS: return D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            case D3D12_BARRIER_LAYOUT_RENDER_TARGET: return D3D12_BARRIER_ACCESS_RENDER_TARGET;
            case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE: return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
            case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ: return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
            case D3D12_BARRIER_LAYOUT_COPY_SOURCE: return D3D12_BARRIER_ACCESS_COPY_SOURCE;
            case D3D12_BARRIER_LAYOUT_COPY_DEST: return D3D12_BARRIER_ACCESS_COPY_DEST;
            default: return D3D12_BARRIER_ACCESS_NO_ACCESS;
            }
        }

        //==============================================================================================================
        // A barrier moving one subresource from `before` into `copy` (COPY_SOURCE or COPY_DEST) for a copy. It waits on every
        // scope for the accesses `before` admits: GPU-based validation rejects SYNC_NONE before a subresource an earlier list
        // touched, and the layer accepts no access in a defined layout only with SYNC_NONE.
        D3D12_TEXTURE_BARRIER intoCopy(ID3D12Resource* texture, uint32_t subresource, D3D12_BARRIER_LAYOUT before, D3D12_BARRIER_LAYOUT copy)
        {
            D3D12_TEXTURE_BARRIER barrier{};
            barrier.SyncBefore = before == D3D12_BARRIER_LAYOUT_UNDEFINED ? D3D12_BARRIER_SYNC_NONE : D3D12_BARRIER_SYNC_ALL;
            barrier.SyncAfter = D3D12_BARRIER_SYNC_COPY;
            barrier.AccessBefore = layoutAccess(before);
            barrier.AccessAfter = layoutAccess(copy);
            barrier.LayoutBefore = before;
            barrier.LayoutAfter = copy;
            barrier.pResource = texture;
            // NumMipLevels 0 makes IndexOrFirstMipLevel a subresource index.
            barrier.Subresources.IndexOrFirstMipLevel = subresource;
            barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
            return barrier;
        }

        //==============================================================================================================
        // A barrier that waits for the copy and moves the subresource from `copy` to `after`, leaving no access behind.
        D3D12_TEXTURE_BARRIER outOfCopy(ID3D12Resource* texture, uint32_t subresource, D3D12_BARRIER_LAYOUT copy, D3D12_BARRIER_LAYOUT after)
        {
            D3D12_TEXTURE_BARRIER barrier{};
            barrier.SyncBefore = D3D12_BARRIER_SYNC_COPY;
            barrier.SyncAfter = D3D12_BARRIER_SYNC_NONE;
            barrier.AccessBefore = layoutAccess(copy);
            barrier.AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS;
            barrier.LayoutBefore = copy;
            barrier.LayoutAfter = after;
            barrier.pResource = texture;
            barrier.Subresources.IndexOrFirstMipLevel = subresource;
            barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
            return barrier;
        }

        //==============================================================================================================
        void recordBarrier(ID3D12GraphicsCommandList9& list, const D3D12_TEXTURE_BARRIER& barrier)
        {
            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = 1;
            group.pTextureBarriers = &barrier;
            list.Barrier(1, &group);
        }

        // The bytes of a pipeline state stream, laid out as the runtime reads them: each subobject starts at a multiple of
        // the pointer size with its type, and its value follows at the value's own alignment.
        struct PipelineStream
        {
            std::array<uint64_t, 128> words{};
            size_t size = 0;
            bool overflow = false; ///< A subobject did not fit and was not written.
        };

        //==============================================================================================================
        template<class Value> void addSubobject(PipelineStream& stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const Value& value)
        {
            const size_t valueAt = alignUp(stream.size + sizeof(type), alignof(Value));
            const size_t end = alignUp(valueAt + sizeof(Value), sizeof(void*));
            if (end > sizeof(stream.words))
            {
                stream.overflow = true;
                return;
            }
            auto* bytes = reinterpret_cast<std::byte*>(stream.words.data());
            std::memcpy(bytes + stream.size, &type, sizeof(type));
            std::memcpy(bytes + valueAt, &value, sizeof(Value));
            stream.size = end;
        }

        //==============================================================================================================
        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc(PipelineStream& stream)
        {
            return D3D12_PIPELINE_STATE_STREAM_DESC{ stream.size, stream.words.data() };
        }

        //==============================================================================================================
        // `ticks`, a timestamp of the queue `clock` calibrates, in seconds of QueryPerformanceCounter: the CPU time of the
        // calibration pair plus the GPU ticks since its GPU time, the arithmetic RojoRHI's submission timing uses.
        double cpuSeconds(const NativeQueueClock& clock, double cpuTicksPerSecond, uint64_t ticks)
        {
            const auto sinceCalibration = static_cast<int64_t>(ticks - clock.gpuCalibration);
            return static_cast<double>(clock.cpuCalibration) / cpuTicksPerSecond + static_cast<double>(sinceCalibration) / clock.gpuTicksPerSecond;
        }

        //==============================================================================================================
        // The index of the entry of `timed` whose list is `list`, searched from `from` onwards and then from the start, so
        // lists committed or ended in the order they were reset are found at once; timed.size() when none is.
        size_t findTimedList(std::span<const NativeTimedList> timed, const ID3D12CommandList* list, size_t from)
        {
            for (size_t step = 0; step < timed.size(); ++step)
            {
                const size_t index = (from + step) % timed.size();
                if (static_cast<const ID3D12CommandList*>(timed[index].list) == list)
                {
                    return index;
                }
            }
            return timed.size();
        }
    } // namespace

    //==================================================================================================================
    void __stdcall nativeMessage(D3D12_MESSAGE_CATEGORY /*category*/, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void* sink)
    {
        const char* name = nullptr;
        switch (severity)
        {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: name = "CORRUPTION"; break;
        case D3D12_MESSAGE_SEVERITY_ERROR: name = "ERROR"; break;
        case D3D12_MESSAGE_SEVERITY_WARNING: name = "WARNING"; break;
        default: return;
        }
        if (sink == nullptr || nativeMessageDenied(id))
        {
            return;
        }
        auto* file = static_cast<std::FILE*>(sink);
        std::fprintf(file, "D3D12 %s %d: %s\n", name, static_cast<int>(id), description != nullptr ? description : "");
        std::fflush(file);
    }

    //==================================================================================================================
    bool nativeMessageDenied(D3D12_MESSAGE_ID id)
    {
        return std::ranges::any_of(kDenied, [id](const DeniedMessage& denied) { return denied.id == id; });
    }

    //==================================================================================================================
    D3D12_BLEND_DESC NativeGraphicsDesc::defaultBlend()
    {
        D3D12_BLEND_DESC desc{};
        desc.AlphaToCoverageEnable = FALSE;
        desc.IndependentBlendEnable = FALSE;
        for (D3D12_RENDER_TARGET_BLEND_DESC& target : desc.RenderTarget)
        {
            target.BlendEnable = FALSE;
            target.LogicOpEnable = FALSE;
            target.SrcBlend = D3D12_BLEND_ONE;
            target.DestBlend = D3D12_BLEND_ZERO;
            target.BlendOp = D3D12_BLEND_OP_ADD;
            target.SrcBlendAlpha = D3D12_BLEND_ONE;
            target.DestBlendAlpha = D3D12_BLEND_ZERO;
            target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            target.LogicOp = D3D12_LOGIC_OP_NOOP;
            target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        return desc;
    }

    //==================================================================================================================
    // The frames' and setups' last values are waited for before anything is released; after a failed wait the GPU may
    // still use any object, and the process is about to report and exit, so nothing is released.
    NativeContext::~NativeContext()
    {
        if (!m_abandoned && m_frameFence != nullptr && m_frameValue > 0)
        {
            if (const std::expected<void, std::string> waited = waitFor(m_frameFence, m_frameValue, "native teardown"); !waited)
            {
                std::fprintf(stderr, "%s; no object is released\n", waited.error().c_str());
            }
        }
        if (m_abandoned)
        {
            (void)m_collector.release();
            (void)m_infoQueue.Detach();
            for (ComPtr<IUnknown>& object : m_owned)
            {
                (void)object.Detach();
            }
            return;
        }
        if (m_cookie != 0)
        {
            m_infoQueue->UnregisterMessageCallback(m_cookie);
        }
        m_infoQueue.Reset();
        while (!m_owned.empty())
        {
            m_owned.pop_back();
        }
        if (m_event != nullptr)
        {
            CloseHandle(m_event);
        }
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeContext::open(const RunOptions& /*options*/)
    {
        if (const std::expected<void, std::string> opened = openDevice(); !opened)
        {
            return opened;
        }
        return createShared();
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeContext::openDevice()
    {
        // The Agility device factory, the only way N loads the runtime, so the executable exports nothing.
        ComPtr<ID3D12SDKConfiguration1> configuration;
        if (const HRESULT got = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&configuration)); FAILED(got))
        {
            return std::unexpected(failed("D3D12GetInterface(CLSID_D3D12SDKConfiguration)", got));
        }
        const std::expected<std::filesystem::path, std::string> directory = executableDirectory();
        if (!directory)
        {
            return std::unexpected(directory.error());
        }
        // The runtime reads the path as UTF-8, with a trailing separator.
        const std::u8string runtime = (*directory / "D3D12").u8string();
        const std::string runtimePath = std::string(reinterpret_cast<const char*>(runtime.data()), runtime.size()) + "\\";
        ComPtr<ID3D12DeviceFactory> factory;
        if (const HRESULT created = configuration->CreateDeviceFactory(kAgilitySdkVersion, runtimePath.c_str(), IID_PPV_ARGS(&factory)); FAILED(created))
        {
            return std::unexpected(failed(std::format("CreateDeviceFactory({}, {})", kAgilitySdkVersion, runtimePath), created));
        }

        // The build's debug features, on this factory only.
        if constexpr (kDebugLayer)
        {
            ComPtr<ID3D12Debug1> debug;
            if (const HRESULT got = factory->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&debug)); FAILED(got))
            {
                return std::unexpected(failed("GetConfigurationInterface(CLSID_D3D12Debug), d3d12SDKLayers.dll beside D3D12Core.dll", got));
            }
            debug->EnableDebugLayer();
            debug->SetEnableGPUBasedValidation(kGpuValidation ? TRUE : FALSE);
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
            if (const HRESULT got = factory->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData, IID_PPV_ARGS(&dred)); FAILED(got))
            {
                return std::unexpected(failed("GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData)", got));
            }
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }

        // No DXGI debug flag: dxgidebug.dll is not redistributable.
        ComPtr<IDXGIFactory6> dxgi;
        if (const HRESULT created = CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgi)); FAILED(created))
        {
            return std::unexpected(failed("CreateDXGIFactory2", created));
        }
        std::vector<ComPtr<IDXGIAdapter1>> candidates;
        if constexpr (kWarpAdapter)
        {
            ComPtr<IDXGIAdapter1> warp;
            if (const HRESULT got = dxgi->EnumWarpAdapter(IID_PPV_ARGS(&warp)); FAILED(got))
            {
                return std::unexpected(failed("EnumWarpAdapter", got));
            }
            candidates.push_back(std::move(warp));
        }
        else
        {
            for (UINT index = 0;; ++index)
            {
                ComPtr<IDXGIAdapter4> adapter;
                const HRESULT got = dxgi->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
                if (got == DXGI_ERROR_NOT_FOUND)
                {
                    break;
                }
                if (FAILED(got))
                {
                    return std::unexpected(failed("EnumAdapterByGpuPreference", got));
                }
                DXGI_ADAPTER_DESC3 desc{};
                if (FAILED(adapter->GetDesc3(&desc)) || (static_cast<UINT>(desc.Flags) & static_cast<UINT>(DXGI_ADAPTER_FLAG3_SOFTWARE)) != 0)
                {
                    continue;
                }
                candidates.emplace_back(std::move(adapter));
            }
        }

        // The first candidate that creates a 12_2 device meeting the floor; the first candidate's failure names the error.
        std::string firstMissing;
        for (ComPtr<IDXGIAdapter1>& candidate : candidates)
        {
            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            ComPtr<ID3D12Device12> opened;
            if (const HRESULT created = factory->CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&opened)); FAILED(created))
            {
                if (firstMissing.empty())
                {
                    firstMissing = failed(std::format("CreateDevice at D3D_FEATURE_LEVEL_12_2 on {}", utf8(desc.Description)), created);
                }
                continue;
            }
            if (const std::optional<std::string> missing = missingFloor(opened.Get()))
            {
                if (firstMissing.empty())
                {
                    firstMissing = std::format("{} does not meet the hardware floor: {}", utf8(desc.Description), *missing);
                }
                continue;
            }
            m_adapterDesc = desc;
            own(factory.Detach());
            own(dxgi.Detach());
            m_adapter = candidate.Get();
            own(candidate.Detach());
            m_device = opened.Get();
            own(opened.Detach(), "bench.device");
            break;
        }
        if (m_device == nullptr)
        {
            return std::unexpected(firstMissing.empty() ? std::string("no adapter of the configured kind") : firstMissing);
        }

        // Under the debug layer, every warning, error and corruption reaches standard error.
        if constexpr (kDebugLayer)
        {
            if (const HRESULT got = m_device->QueryInterface(IID_PPV_ARGS(&m_infoQueue)); FAILED(got))
            {
                return std::unexpected(failed("QueryInterface(ID3D12InfoQueue1) with the debug layer on", got));
            }
            // The runtime's default storage filter drops some messages, and a callback registered without IGNORE_FILTERS
            // obeys it; clearing it lets every message reach the callback, which drops only what kDenied names.
            m_infoQueue->ClearStorageFilter();
            if (const HRESULT registered = m_infoQueue->RegisterMessageCallback(nativeMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, stderr, &m_cookie);
                FAILED(registered))
            {
                return std::unexpected(failed("ID3D12InfoQueue1::RegisterMessageCallback", registered));
            }
        }
        return {};
    }

    //==================================================================================================================
    // The queues, the global root signature, both shader-visible heaps with the null views, the fences and the setup list.
    std::expected<void, std::string> NativeContext::createShared()
    {
        for (const auto& [type, label, queue] : { std::tuple{ D3D12_COMMAND_LIST_TYPE_DIRECT, "bench.graphics", &m_graphics },
                                                  std::tuple{ D3D12_COMMAND_LIST_TYPE_COMPUTE, "bench.compute", &m_compute } })
        {
            D3D12_COMMAND_QUEUE_DESC desc{};
            desc.Type = type;
            desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            const HRESULT created = m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue));
            ++m_calls.host;
            if (FAILED(created))
            {
                return std::unexpected(failed(std::format("CreateCommandQueue for {}", label), created));
            }
            own(*queue, label);
        }

        // The global root signature of the module's ABI: the root SRV at t0 and the draw id at b1, every stage visible,
        // both heaps indexed directly and nothing bound to the input assembler.
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
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rootDesc.Desc_1_1.NumParameters = 2;
        rootDesc.Desc_1_1.pParameters = parameters;
        rootDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
        // Serialized through the device's configuration interface, so the Agility runtime owns the format.
        ComPtr<ID3D12DeviceConfiguration> deviceConfiguration;
        if (const HRESULT got = m_device->QueryInterface(IID_PPV_ARGS(&deviceConfiguration)); FAILED(got))
        {
            return std::unexpected(failed("QueryInterface(ID3D12DeviceConfiguration)", got));
        }
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        const HRESULT serialized = deviceConfiguration->SerializeVersionedRootSignature(&rootDesc, &blob, &errors);
        ++m_calls.host;
        if (FAILED(serialized))
        {
            const std::string_view detail =
                errors ? std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : std::string_view();
            return std::unexpected(std::format("{}: {}", failed("SerializeVersionedRootSignature", serialized), detail));
        }
        const HRESULT rooted = m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root));
        ++m_calls.host;
        if (FAILED(rooted))
        {
            return std::unexpected(failed("CreateRootSignature", rooted));
        }
        own(m_root, "bench.root");

        const std::expected<ID3D12DescriptorHeap*, std::string> views =
            descriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewHeapSize, true, "bench.views");
        if (!views)
        {
            return std::unexpected(views.error());
        }
        const std::expected<ID3D12DescriptorHeap*, std::string> samplers =
            descriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize, true, "bench.samplers");
        if (!samplers)
        {
            return std::unexpected(samplers.error());
        }
        m_heaps = { *views, *samplers };
        m_viewIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_samplerIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        m_viewCpuStart = (*views)->GetCPUDescriptorHandleForHeapStart();
        m_viewGpuStart = (*views)->GetGPUDescriptorHandleForHeapStart();
        m_samplerGpuStart = (*samplers)->GetGPUDescriptorHandleForHeapStart();
        m_samplerCpuStart = (*samplers)->GetCPUDescriptorHandleForHeapStart();
        m_calls.host += 6;
        // The null raw views at slots 0 and 1, which the zero Addr reaches.
        const D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = rawShaderResource(1);
        m_device->CreateShaderResourceView(nullptr, &nullSrv, viewCpu(0));
        const D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav = rawUnorderedAccess(1);
        m_device->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, viewCpu(1));
        m_calls.host += 2;

        const std::expected<ID3D12Fence*, std::string> frames = fence("bench.frame");
        if (!frames)
        {
            return std::unexpected(frames.error());
        }
        m_frameFence = *frames;
        const std::expected<ID3D12Fence*, std::string> setups = fence("bench.setup");
        if (!setups)
        {
            return std::unexpected(setups.error());
        }
        m_setupFence = *setups;
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_event == nullptr)
        {
            return std::unexpected(std::format("CreateEventW failed with error {}", GetLastError()));
        }
        const std::expected<NativeList, std::string> setupList = commandList(D3D12_COMMAND_LIST_TYPE_DIRECT, "bench.setup");
        if (!setupList)
        {
            return std::unexpected(setupList.error());
        }
        m_setup = *setupList;
        return {};
    }

    //==================================================================================================================
    ID3D12Device12* NativeContext::device() const
    {
        return m_device;
    }

    //==================================================================================================================
    IDXGIAdapter1* NativeContext::adapter() const
    {
        return m_adapter;
    }

    //==================================================================================================================
    std::string NativeContext::adapterName() const
    {
        return utf8(m_adapterDesc.Description);
    }

    //==================================================================================================================
    const DXGI_ADAPTER_DESC1& NativeContext::adapterDesc() const
    {
        return m_adapterDesc;
    }

    //==================================================================================================================
    ID3D12CommandQueue* NativeContext::graphicsQueue() const
    {
        return m_graphics;
    }

    //==================================================================================================================
    ID3D12CommandQueue* NativeContext::computeQueue() const
    {
        return m_compute;
    }

    //==================================================================================================================
    ID3D12RootSignature* NativeContext::rootSignature() const
    {
        return m_root;
    }

    //==================================================================================================================
    ID3D12DescriptorHeap* NativeContext::viewHeap() const
    {
        return m_heaps[0];
    }

    //==================================================================================================================
    ID3D12DescriptorHeap* NativeContext::samplerHeap() const
    {
        return m_heaps[1];
    }

    //==================================================================================================================
    std::span<ID3D12DescriptorHeap* const> NativeContext::heaps() const
    {
        return m_heaps;
    }

    //==================================================================================================================
    ID3D12Fence* NativeContext::frameFence() const
    {
        return m_frameFence;
    }

    //==================================================================================================================
    DWORD NativeContext::messageCookie() const
    {
        return m_cookie;
    }

    //==================================================================================================================
    bool NativeContext::frameFailed() const
    {
        return !m_frameError.empty();
    }

    //==================================================================================================================
    NativeCalls& NativeContext::calls()
    {
        return m_calls;
    }

    //==================================================================================================================
    CallSnapshot NativeContext::callSnapshot() const
    {
        return CallSnapshot{ .calls = m_calls.record + m_calls.submit + m_calls.host, .barriers = m_calls.barriers };
    }

    //==================================================================================================================
    FeedbackCollector& NativeContext::collector()
    {
        return *m_collector;
    }

    //==================================================================================================================
    void NativeContext::own(ID3D12Object* object, std::string_view label)
    {
        object->SetName(widen(label).c_str());
        own(static_cast<IUnknown*>(object));
    }

    //==================================================================================================================
    void NativeContext::own(IUnknown* object)
    {
        m_owned.emplace_back();
        m_owned.back().Attach(object);
    }

    //==================================================================================================================
    // A buffer's width is its size rounded up to four bytes, so its raw views cover every byte; a DEFAULT buffer allows
    // unordered access, so a module Addr can store to it.
    std::expected<NativeBuffer, std::string> NativeContext::committedBuffer(D3D12_HEAP_TYPE heap, uint64_t size, std::string_view label)
    {
        if (size == 0)
        {
            return std::unexpected(std::format("buffer {}: a buffer holds at least one byte", label));
        }
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = heap;
        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = (size + 3) & ~uint64_t{ 3 };
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = heap == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* resource = nullptr;
        const HRESULT created = m_device->CreateCommittedResource3(
            &properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&resource)
        );
        ++m_calls.host;
        if (FAILED(created))
        {
            return std::unexpected(failed(std::format("CreateCommittedResource3 for buffer {} of {} bytes", label, size), created));
        }
        own(resource, label);
        return NativeBuffer{ .resource = resource, .address = resource->GetGPUVirtualAddress(), .data = nullptr, .size = size };
    }

    //==================================================================================================================
    std::expected<NativeBuffer, std::string> NativeContext::defaultBuffer(uint64_t size, std::string_view label, std::span<const std::byte> initial)
    {
        if (initial.size() > size)
        {
            return std::unexpected(std::format("buffer {}: {} bytes cannot hold {} initial bytes", label, size, initial.size()));
        }
        const std::expected<NativeBuffer, std::string> buffer = committedBuffer(D3D12_HEAP_TYPE_DEFAULT, size, label);
        if (!buffer || initial.empty())
        {
            return buffer;
        }
        const std::expected<NativeBuffer, std::string> staging = uploadBuffer(initial.size(), std::string(label) + ".staging", initial);
        if (!staging)
        {
            return std::unexpected(staging.error());
        }
        const std::expected<void, std::string> copied = runSetup([&](ID3D12GraphicsCommandList9& list) {
            list.CopyBufferRegion(buffer->resource, 0, staging->resource, 0, initial.size());
            ++m_calls.record;
        });
        if (!copied)
        {
            return std::unexpected(std::format("buffer {}: initial upload: {}", label, copied.error()));
        }
        return buffer;
    }

    //==================================================================================================================
    std::expected<NativeBuffer, std::string> NativeContext::uploadBuffer(uint64_t size, std::string_view label, std::span<const std::byte> initial)
    {
        if (initial.size() > size)
        {
            return std::unexpected(std::format("buffer {}: {} bytes cannot hold {} initial bytes", label, size, initial.size()));
        }
        std::expected<NativeBuffer, std::string> buffer = committedBuffer(D3D12_HEAP_TYPE_UPLOAD, size, label);
        if (!buffer)
        {
            return buffer;
        }
        // The CPU reads nothing back, so the read range is empty.
        const D3D12_RANGE read{ 0, 0 };
        void* mapped = nullptr;
        const HRESULT mappedResult = buffer->resource->Map(0, &read, &mapped);
        ++m_calls.host;
        if (FAILED(mappedResult))
        {
            return std::unexpected(failed(std::format("Map of buffer {}", label), mappedResult));
        }
        buffer->data = static_cast<std::byte*>(mapped);
        std::memset(buffer->data, 0, static_cast<size_t>(size));
        if (!initial.empty())
        {
            std::memcpy(buffer->data, initial.data(), initial.size());
        }
        return buffer;
    }

    //==================================================================================================================
    std::expected<NativeBuffer, std::string> NativeContext::readbackBuffer(uint64_t size, std::string_view label)
    {
        return committedBuffer(D3D12_HEAP_TYPE_READBACK, size, label);
    }

    //==================================================================================================================
    std::expected<ID3D12Resource*, std::string> NativeContext::texture(
        const D3D12_RESOURCE_DESC1& desc, D3D12_BARRIER_LAYOUT layout, std::string_view label, const D3D12_CLEAR_VALUE* clear
    )
    {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource* resource = nullptr;
        const HRESULT created =
            m_device->CreateCommittedResource3(&properties, D3D12_HEAP_FLAG_NONE, &desc, layout, clear, nullptr, 0, nullptr, IID_PPV_ARGS(&resource));
        ++m_calls.host;
        if (FAILED(created))
        {
            return std::unexpected(failed(std::format("CreateCommittedResource3 for texture {}", label), created));
        }
        own(resource, label);
        return resource;
    }

    //==================================================================================================================
    std::expected<ID3D12DescriptorHeap*, std::string> NativeContext::descriptorHeap(
        D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shaderVisible, std::string_view label
    )
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type;
        desc.NumDescriptors = count;
        desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ID3D12DescriptorHeap* heap = nullptr;
        const HRESULT created = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
        ++m_calls.host;
        if (FAILED(created))
        {
            return std::unexpected(failed(std::format("CreateDescriptorHeap for {} of {} descriptors", label, count), created));
        }
        own(heap, label);
        return heap;
    }

    //==================================================================================================================
    std::expected<ID3D12QueryHeap*, std::string> NativeContext::timestampHeap(uint32_t count, std::string_view label)
    {
        D3D12_QUERY_HEAP_DESC desc{};
        desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        desc.Count = count;
        ID3D12QueryHeap* heap = nullptr;
        const HRESULT created = m_device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap));
        ++m_calls.host;
        if (FAILED(created))
        {
            return std::unexpected(failed(std::format("CreateQueryHeap for {} of {} timestamps", label, count), created));
        }
        own(heap, label);
        return heap;
    }

    //==================================================================================================================
    std::expected<ID3D12Fence*, std::string> NativeContext::fence(std::string_view label)
    {
        ID3D12Fence* created = nullptr;
        const HRESULT result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&created));
        ++m_calls.host;
        if (FAILED(result))
        {
            return std::unexpected(failed(std::format("CreateFence for {}", label), result));
        }
        own(created, label);
        return created;
    }

    //==================================================================================================================
    std::expected<NativeList, std::string> NativeContext::commandList(D3D12_COMMAND_LIST_TYPE type, std::string_view label)
    {
        NativeList created;
        const HRESULT allocated = m_device->CreateCommandAllocator(type, IID_PPV_ARGS(&created.allocator));
        ++m_calls.host;
        if (FAILED(allocated))
        {
            return std::unexpected(failed(std::format("CreateCommandAllocator for {}", label), allocated));
        }
        own(created.allocator, std::string(label) + ".allocator");
        const HRESULT listed = m_device->CreateCommandList1(0, type, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&created.list));
        ++m_calls.host;
        if (FAILED(listed))
        {
            return std::unexpected(failed(std::format("CreateCommandList1 for {}", label), listed));
        }
        own(created.list, label);
        return created;
    }

    //==================================================================================================================
    std::expected<uint32_t, std::string> NativeContext::claimViews(uint32_t count, std::string_view what)
    {
        if (m_heaps[0] == nullptr || count > kViewHeapSize - m_nextView)
        {
            return std::unexpected(
                std::format("{}: the view heap of {} descriptors has {} left, not {}", what, kViewHeapSize, kViewHeapSize - m_nextView, count)
            );
        }
        const uint32_t index = m_nextView;
        m_nextView += count;
        return index;
    }

    //==================================================================================================================
    std::expected<uint32_t, std::string> NativeContext::bufferViews(ID3D12Resource* buffer)
    {
        const D3D12_RESOURCE_DESC desc = buffer->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            return std::unexpected(std::string("buffer views: the resource is not a buffer"));
        }
        const std::expected<uint32_t, std::string> index = claimViews(2, "buffer views");
        if (!index)
        {
            return index;
        }
        const uint64_t elements = std::min(desc.Width / 4, kRawViewElements);
        const D3D12_SHADER_RESOURCE_VIEW_DESC srv = rawShaderResource(elements);
        m_device->CreateShaderResourceView(buffer, &srv, viewCpu(*index));
        ++m_calls.host;
        if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != D3D12_RESOURCE_FLAG_NONE)
        {
            const D3D12_UNORDERED_ACCESS_VIEW_DESC uav = rawUnorderedAccess(elements);
            m_device->CreateUnorderedAccessView(buffer, nullptr, &uav, viewCpu(*index + 1));
            ++m_calls.host;
        }
        return index;
    }

    //==================================================================================================================
    std::expected<uint32_t, std::string> NativeContext::shaderResourceView(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc)
    {
        const std::expected<uint32_t, std::string> index = claimViews(1, "shader resource view");
        if (index)
        {
            m_device->CreateShaderResourceView(resource, &desc, viewCpu(*index));
            ++m_calls.host;
        }
        return index;
    }

    //==================================================================================================================
    std::expected<uint32_t, std::string> NativeContext::unorderedAccessView(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc)
    {
        const std::expected<uint32_t, std::string> index = claimViews(1, "unordered access view");
        if (index)
        {
            m_device->CreateUnorderedAccessView(resource, nullptr, &desc, viewCpu(*index));
            ++m_calls.host;
        }
        return index;
    }

    //==================================================================================================================
    std::expected<uint32_t, std::string> NativeContext::sampler(const D3D12_SAMPLER_DESC& desc)
    {
        if (m_heaps[1] == nullptr || m_nextSampler == kSamplerHeapSize)
        {
            return std::unexpected(std::format("sampler: the sampler heap of {} descriptors is full", kSamplerHeapSize));
        }
        const uint32_t index = m_nextSampler++;
        m_device->CreateSampler(&desc, D3D12_CPU_DESCRIPTOR_HANDLE{ m_samplerCpuStart.ptr + SIZE_T{ index } * m_samplerIncrement });
        ++m_calls.host;
        return index;
    }

    //==================================================================================================================
    D3D12_CPU_DESCRIPTOR_HANDLE NativeContext::viewCpu(uint32_t index) const
    {
        return D3D12_CPU_DESCRIPTOR_HANDLE{ m_viewCpuStart.ptr + SIZE_T{ index } * m_viewIncrement };
    }

    //==================================================================================================================
    D3D12_GPU_DESCRIPTOR_HANDLE NativeContext::viewGpu(uint32_t index) const
    {
        return D3D12_GPU_DESCRIPTOR_HANDLE{ m_viewGpuStart.ptr + UINT64{ index } * m_viewIncrement };
    }

    //==================================================================================================================
    D3D12_GPU_DESCRIPTOR_HANDLE NativeContext::samplerGpu(uint32_t index) const
    {
        return D3D12_GPU_DESCRIPTOR_HANDLE{ m_samplerGpuStart.ptr + UINT64{ index } * m_samplerIncrement };
    }

    //==================================================================================================================
    std::expected<D3D12_SHADER_BYTECODE, std::string> NativeContext::library(std::string_view name, std::string_view entry)
    {
        const std::expected<std::filesystem::path, std::string> directory = executableDirectory();
        if (!directory)
        {
            return std::unexpected(std::format("library {}: {}", name, directory.error()));
        }
        const std::filesystem::path shaders = *directory / "Shaders";
        std::filesystem::path path = shaders / (std::string(name) + ".dxil");
        std::error_code error;
        if (!entry.empty())
        {
            const std::filesystem::path perEntry = shaders / std::format("{}.{}.dxil", name, entry);
            if (std::filesystem::exists(perEntry, error))
            {
                path = perEntry;
            }
        }
        if (!std::filesystem::exists(path, error))
        {
            return std::unexpected(
                std::format("library {}: {} does not exist; the rojorhi.slang rule, or the rule of a hand-written kernel, builds it", name, path.string())
            );
        }
        std::ifstream file(path, std::ios::binary);
        std::vector<std::byte>& bytes = m_shaders.emplace_back();
        std::transform(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>(), std::back_inserter(bytes), [](char c) {
            return static_cast<std::byte>(c);
        });
        if (file.bad() || bytes.empty())
        {
            return std::unexpected(std::format("library {}: {} cannot be read", name, path.string()));
        }
        return D3D12_SHADER_BYTECODE{ bytes.data(), bytes.size() };
    }

    //==================================================================================================================
    std::expected<ID3D12PipelineState*, std::string> NativeContext::createPipeline(const D3D12_PIPELINE_STATE_STREAM_DESC& desc, std::string_view label)
    {
        ID3D12PipelineState* pipeline = nullptr;
        const HRESULT created = m_device->CreatePipelineState(&desc, IID_PPV_ARGS(&pipeline));
        ++m_calls.host;
        if (FAILED(created))
        {
            return std::unexpected(failed(std::format("CreatePipelineState for {}", label), created));
        }
        own(pipeline, label);
        return pipeline;
    }

    //==================================================================================================================
    std::expected<ID3D12PipelineState*, std::string> NativeContext::computePipeline(
        D3D12_SHADER_BYTECODE shader, std::string_view label, ID3D12RootSignature* root
    )
    {
        PipelineStream stream;
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root != nullptr ? root : m_root);
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, shader);
        return createPipeline(streamDesc(stream), label);
    }

    //==================================================================================================================
    std::expected<ID3D12PipelineState*, std::string> NativeContext::graphicsPipeline(const NativeGraphicsDesc& desc, std::string_view label)
    {
        D3D12_RT_FORMAT_ARRAY formats{};
        if (desc.colorFormats.size() > std::size(formats.RTFormats))
        {
            return std::unexpected(std::format("graphics pipeline {}: {} color formats, at most 8", label, desc.colorFormats.size()));
        }
        std::ranges::copy(desc.colorFormats, formats.RTFormats);
        formats.NumRenderTargets = static_cast<UINT>(desc.colorFormats.size());

        PipelineStream stream;
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, desc.root != nullptr ? desc.root : m_root);
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, desc.vertex);
        // Every vertex is pulled, so nothing is bound to the input assembler.
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC{ nullptr, 0 });
        if (desc.pixel.BytecodeLength > 0)
        {
            addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, desc.pixel);
        }
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, desc.blend);
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, std::numeric_limits<UINT>::max());
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2, desc.rasterizer);
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2, desc.depthStencil);
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, desc.topology);
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, desc.depthFormat);
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC{ 1, 0 });
        addSubobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, desc.flags);
        if (stream.overflow)
        {
            return std::unexpected(std::format("graphics pipeline {}: the stream outgrew its storage", label));
        }
        return createPipeline(streamDesc(stream), label);
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeContext::prepareFrames(const NativeFrameShape& shape)
    {
        if (!m_slots.empty() || shape.lists == 0)
        {
            return std::unexpected(std::format("frame slots: prepared twice or with {} lists", shape.lists));
        }
        m_slots.resize(kFrameSlots);
        for (uint32_t index = 0; index < kFrameSlots; ++index)
        {
            Slot& slot = m_slots[index];
            for (uint32_t list = 0; list < shape.lists; ++list)
            {
                const std::expected<NativeList, std::string> created =
                    commandList(D3D12_COMMAND_LIST_TYPE_DIRECT, std::format("bench.list.{}.{}", index, list));
                if (!created)
                {
                    return std::unexpected(std::format("frame slot {}: {}", index, created.error()));
                }
                slot.allocators.push_back(created->allocator);
                slot.lists.push_back(created->list);
            }
            if (shape.arenaBytes > 0)
            {
                const std::expected<NativeBuffer, std::string> arena = uploadBuffer(shape.arenaBytes, std::format("bench.arena.{}", index));
                if (!arena)
                {
                    return std::unexpected(arena.error());
                }
                slot.arena = arena->resource;
                slot.arenaData = arena->data;
                slot.arenaAddress = arena->address;
                slot.arenaSize = shape.arenaBytes;
            }
        }
        // The timestamps count nowhere: the counts their creation adds are taken back out.
        const NativeCalls counted = m_calls;
        const std::expected<void, std::string> timed = prepareTimestamps(shape.lists + kExtraTimedLists);
        m_calls = counted;
        return timed;
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeContext::prepareTimestamps(uint32_t pairs)
    {
        constexpr std::array<std::string_view, 2> kNames{ "direct", "compute" };
        const std::array<ID3D12CommandQueue*, 2> queues{ m_graphics, m_compute };
        for (size_t queue = 0; queue < queues.size(); ++queue)
        {
            UINT64 frequency = 0;
            if (const HRESULT read = queues[queue]->GetTimestampFrequency(&frequency); FAILED(read))
            {
                return std::unexpected(failed(std::format("GetTimestampFrequency of the {} queue", kNames[queue]), read));
            }
            UINT64 gpuTimestamp = 0;
            UINT64 cpuTimestamp = 0;
            if (const HRESULT read = queues[queue]->GetClockCalibration(&gpuTimestamp, &cpuTimestamp); FAILED(read))
            {
                return std::unexpected(failed(std::format("GetClockCalibration of the {} queue", kNames[queue]), read));
            }
            m_clocks[queue] =
                NativeQueueClock{ .gpuTicksPerSecond = static_cast<double>(frequency), .gpuCalibration = gpuTimestamp, .cpuCalibration = cpuTimestamp };
        }
        // QueryPerformanceFrequency cannot fail on Windows XP or later.
        LARGE_INTEGER cpuFrequency{};
        QueryPerformanceFrequency(&cpuFrequency);
        m_cpuTicksPerSecond = static_cast<double>(cpuFrequency.QuadPart);

        for (uint32_t index = 0; index < kFrameSlots; ++index)
        {
            NativeSlotTimestamps& timestamps = m_slots[index].timestamps;
            const std::expected<ID3D12QueryHeap*, std::string> heap = timestampHeap(2 * pairs, std::format("bench.timestamps.{}", index));
            if (!heap)
            {
                return std::unexpected(heap.error());
            }
            timestamps.queries = *heap;
            timestamps.pairs = pairs;
            // One buffer per queue, so no resource is written by both queues at once.
            for (size_t queue = 0; queue < queues.size(); ++queue)
            {
                const std::string label = std::format("bench.timestamps.{}.{}.readback", index, kNames[queue]);
                const std::expected<NativeBuffer, std::string> readback = readbackBuffer(uint64_t{ 2 } * pairs * sizeof(uint64_t), label);
                if (!readback)
                {
                    return std::unexpected(readback.error());
                }
                // The buffer stays mapped: the CPU reads it only after the frame fence shows the frame's resolves complete.
                void* mapped = nullptr;
                if (const HRESULT read = readback->resource->Map(0, nullptr, &mapped); FAILED(read))
                {
                    return std::unexpected(failed(std::format("Map of {}", label), read));
                }
                timestamps.readbacks[queue] = readback->resource;
                timestamps.ticks[queue] = static_cast<const uint64_t*>(mapped);
            }
        }
        m_timedLists.reserve(pairs);
        m_timed.reserve(pairs);
        return {};
    }

    //==================================================================================================================
    void NativeContext::startTimestamps(ID3D12GraphicsCommandList9* list, D3D12_COMMAND_LIST_TYPE type)
    {
        if (m_frameSlot == nullptr)
        {
            if (m_frameError.empty())
            {
                m_frameError = "a list wrote its first timestamp outside a frame";
            }
            return;
        }
        const NativeSlotTimestamps& timestamps = m_frameSlot->timestamps;
        if (m_timedLists.size() == timestamps.pairs)
        {
            if (m_frameError.empty())
            {
                m_frameError = std::format("the frame recorded more than {} lists, the timestamp pairs its slot holds", timestamps.pairs);
            }
            return;
        }
        const auto pair = static_cast<uint32_t>(m_timedLists.size());
        list->EndQuery(timestamps.queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * pair);
        const uint32_t queue = type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1 : 0;
        m_timedLists.push_back(NativeTimedList{ .list = list, .pair = pair, .queue = queue, .ended = false });
    }

    //==================================================================================================================
    void NativeContext::endTimestamps(ID3D12GraphicsCommandList9* list)
    {
        if (m_frameSlot == nullptr || m_timedLists.empty())
        {
            return;
        }
        const size_t found = findTimedList(m_timedLists, list, m_timedLists.size() - 1);
        if (found == m_timedLists.size() || m_timedLists[found].ended)
        {
            return;
        }
        const NativeSlotTimestamps& timestamps = m_frameSlot->timestamps;
        const UINT first = 2 * m_timedLists[found].pair;
        list->EndQuery(timestamps.queries, D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
        ID3D12Resource* readback = timestamps.readbacks[m_timedLists[found].queue];
        list->ResolveQueryData(timestamps.queries, D3D12_QUERY_TYPE_TIMESTAMP, first, 2, readback, uint64_t{ first } * sizeof(uint64_t));
        m_timedLists[found].ended = true;
    }

    //==================================================================================================================
    // Once the fence holds the value the slot's last frame signalled at its end, that frame's lists and arena are free to
    // reuse. After a failed wait the GPU may still use them, so the allocators are not reset, nothing is released at
    // teardown, and endFrame fails. A wait that succeeds allocates nothing.
    NativeContext::Slot& NativeContext::beginFrame(uint32_t index, uint32_t submissions)
    {
        if (m_slots.empty())
        {
            m_frameError = std::format("native frame {} began before prepareFrames", index);
            m_slots.resize(kFrameSlots);
        }
        Slot& slot = m_slots[index % kFrameSlots];
        m_frameSlot = &slot;
        m_submissions = submissions;
        m_committed = 0;
        m_timed.clear();
        m_timedLists.clear();
        m_commitCursor = 0;
        m_collector->begin(submissions);
        if (const std::expected<void, std::string> waited = waitFor(m_frameFence, slot.frameValue, "the frame fence"); !waited)
        {
            m_frameError = std::format("frame slot {}: {}", index % kFrameSlots, waited.error());
        }
        else
        {
            for (ID3D12CommandAllocator* allocator : slot.allocators)
            {
                const HRESULT reset = allocator->Reset();
                ++m_calls.host;
                if (FAILED(reset) && m_frameError.empty())
                {
                    m_frameError = failed(std::format("frame slot {}: ID3D12CommandAllocator::Reset", index % kFrameSlots), reset);
                }
            }
        }
        // A frame that already failed keeps the slot's arena as it is: the GPU may still read it, and nothing is executed.
        if (m_frameError.empty())
        {
            slot.arenaUsed = 0;
            slot.overrunAt = 0;
            slot.overrunBytes = 0;
        }
        return slot;
    }

    //==================================================================================================================
    void NativeContext::commit(ID3D12CommandQueue* queue, std::span<ID3D12CommandList* const> lists)
    {
        ++m_committed;
        if (m_frameSlot == nullptr && m_frameError.empty())
        {
            m_frameError = "a submission was committed outside a frame";
        }
        if (!m_frameError.empty() || m_frameSlot->overrunBytes != 0)
        {
            return;
        }
        const size_t first = lists.empty() ? m_timedLists.size() : findTimedList(m_timedLists, lists.front(), m_commitCursor);
        const size_t last = lists.empty() ? m_timedLists.size() : findTimedList(m_timedLists, lists.back(), first);
        if (first == m_timedLists.size() || last == m_timedLists.size() || !m_timedLists[first].ended || !m_timedLists[last].ended)
        {
            m_frameError = "a submission's first or last list wrote no timestamps, so its GPU span is unknown";
            return;
        }
        m_commitCursor = last + 1;
        queue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
        ++m_calls.submit;
        const uint32_t onQueue = queue == m_compute ? 1 : 0;
        m_timed.push_back(TimedSubmission{ .queue = onQueue, .start = 2 * m_timedLists[first].pair, .end = 2 * m_timedLists[last].pair + 1 });
    }

    //==================================================================================================================
    std::expected<std::optional<double>, std::string> NativeContext::endFrame()
    {
        Slot* slot = std::exchange(m_frameSlot, nullptr);
        const uint64_t value = m_frameValue + 1;
        const HRESULT signalled = m_graphics->Signal(m_frameFence, value);
        ++m_calls.submit;
        if (SUCCEEDED(signalled))
        {
            m_frameValue = value;
            if (slot != nullptr)
            {
                slot->frameValue = value;
            }
        }
        if (!m_frameError.empty())
        {
            return std::unexpected(std::exchange(m_frameError, std::string()));
        }
        if (FAILED(signalled))
        {
            return std::unexpected(failed("ID3D12CommandQueue::Signal of the frame fence", signalled));
        }
        if (slot != nullptr && slot->overrunBytes != 0)
        {
            return std::unexpected(
                std::format(
                    "the frame-data arena of {} bytes cannot hold {} bytes at offset {}, so the frame's submissions were not executed",
                    slot->arenaSize,
                    slot->overrunBytes,
                    slot->overrunAt
                )
            );
        }
        if (m_committed != m_submissions)
        {
            return std::unexpected(std::format("the frame announced {} submissions and made {}", m_submissions, m_committed));
        }
        if (slot == nullptr)
        {
            return std::optional<double>{};
        }
        return reportTimestamps(*slot);
    }

    //==================================================================================================================
    std::expected<std::optional<double>, std::string> NativeContext::reportTimestamps(const Slot& slot)
    {
        if (const std::expected<void, std::string> waited = waitFor(m_frameFence, m_frameValue, "the frame's timestamps", false); !waited)
        {
            return std::unexpected(waited.error());
        }
        for (const TimedSubmission& submission : m_timed)
        {
            const NativeQueueClock& clock = m_clocks[submission.queue];
            const uint64_t* ticks = slot.timestamps.ticks[submission.queue];
            const double start = cpuSeconds(clock, m_cpuTicksPerSecond, ticks[submission.start]);
            const double end = cpuSeconds(clock, m_cpuTicksPerSecond, ticks[submission.end]);
            m_collector->report(start, end, false);
        }
        const std::expected<double, std::string> span = m_collector->wait();
        if (!span)
        {
            return std::unexpected(span.error());
        }
        return std::optional<double>{ *span };
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeContext::waitFor(ID3D12Fence* target, uint64_t value, std::string_view what, bool counted)
    {
        if (m_abandoned)
        {
            return std::unexpected(std::format("{}: an earlier wait timed out, so the context waits no more", what));
        }
        if (value == 0)
        {
            return {};
        }
        const HRESULT armed = target->SetEventOnCompletion(value, m_event);
        if (counted)
        {
            ++m_calls.host;
        }
        if (FAILED(armed))
        {
            m_abandoned = true;
            return std::unexpected(failed(std::format("{}: SetEventOnCompletion({})", what, value), armed));
        }
        if (WaitForSingleObject(m_event, kWaitMilliseconds) != WAIT_OBJECT_0)
        {
            m_abandoned = true;
            return std::unexpected(std::format("{}: the fence did not reach {} within {} ms", what, value, kWaitMilliseconds));
        }
        return {};
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeContext::runSetup(const std::function<void(ID3D12GraphicsCommandList9& list)>& record)
    {
        if (m_abandoned)
        {
            return std::unexpected(std::string("setup: an earlier wait timed out, so the context submits nothing more"));
        }
        if (const std::expected<void, std::string> frames = waitFor(m_frameFence, m_frameValue, "setup: the frames before it"); !frames)
        {
            return frames;
        }
        const HRESULT allocatorReset = m_setup.allocator->Reset();
        const HRESULT listReset = m_setup.list->Reset(m_setup.allocator, nullptr);
        m_calls.host += 2;
        if (FAILED(allocatorReset) || FAILED(listReset))
        {
            return std::unexpected(failed("setup: Reset of the setup allocator and list", FAILED(allocatorReset) ? allocatorReset : listReset));
        }
        record(*m_setup.list);
        const HRESULT closed = m_setup.list->Close();
        ++m_calls.record;
        if (FAILED(closed))
        {
            return std::unexpected(failed("setup: ID3D12GraphicsCommandList::Close", closed));
        }
        ID3D12CommandList* const lists[] = { m_setup.list };
        m_graphics->ExecuteCommandLists(1, lists);
        const HRESULT signalled = m_graphics->Signal(m_setupFence, m_setupValue + 1);
        m_calls.submit += 2;
        if (FAILED(signalled))
        {
            return std::unexpected(failed("setup: ID3D12CommandQueue::Signal", signalled));
        }
        ++m_setupValue;
        return waitFor(m_setupFence, m_setupValue, "setup");
    }

    //==================================================================================================================
    std::expected<std::vector<std::byte>, std::string> NativeContext::readBuffer(ID3D12Resource* source, uint64_t offset, uint64_t size)
    {
        const std::expected<NativeBuffer, std::string> target = readbackBuffer(size, "bench.readback");
        if (!target)
        {
            return std::unexpected(target.error());
        }
        const std::expected<void, std::string> copied = runSetup([&](ID3D12GraphicsCommandList9& list) {
            list.CopyBufferRegion(target->resource, 0, source, offset, size);
            ++m_calls.record;
        });
        if (!copied)
        {
            return std::unexpected("readback: " + copied.error());
        }
        const D3D12_RANGE read{ 0, static_cast<SIZE_T>(size) };
        void* mapped = nullptr;
        const HRESULT mappedResult = target->resource->Map(0, &read, &mapped);
        ++m_calls.host;
        if (FAILED(mappedResult))
        {
            return std::unexpected(failed("readback: Map", mappedResult));
        }
        const auto* bytes = static_cast<const std::byte*>(mapped);
        std::vector<std::byte> result(bytes, bytes + size);
        const D3D12_RANGE written{ 0, 0 };
        target->resource->Unmap(0, &written);
        ++m_calls.host;
        return result;
    }

    //==================================================================================================================
    std::expected<std::vector<std::byte>, std::string> NativeContext::readTexture(ID3D12Resource* image, D3D12_BARRIER_LAYOUT layout, uint32_t subresource)
    {
        const D3D12_RESOURCE_DESC desc = image->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 rowSize = 0;
        UINT64 total = 0;
        m_device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &rows, &rowSize, &total);
        ++m_calls.host;
        if (total == UINT64_MAX)
        {
            return std::unexpected(std::format("texture readback: the texture has no subresource {}", subresource));
        }
        const std::expected<NativeBuffer, std::string> target = readbackBuffer(total, "bench.readback");
        if (!target)
        {
            return std::unexpected(target.error());
        }
        const std::expected<void, std::string> copied = runSetup([&](ID3D12GraphicsCommandList9& list) {
            const bool moves = layout != D3D12_BARRIER_LAYOUT_COPY_SOURCE;
            if (moves)
            {
                recordBarrier(list, intoCopy(image, subresource, layout, D3D12_BARRIER_LAYOUT_COPY_SOURCE));
            }
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = target->resource;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = image;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = subresource;
            list.CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            if (moves)
            {
                recordBarrier(list, outOfCopy(image, subresource, D3D12_BARRIER_LAYOUT_COPY_SOURCE, layout));
            }
            const uint64_t barriers = moves ? 2 : 0;
            m_calls.record += 1 + barriers;
            m_calls.barriers += barriers;
        });
        if (!copied)
        {
            return std::unexpected("texture readback: " + copied.error());
        }
        const D3D12_RANGE read{ 0, static_cast<SIZE_T>(total) };
        void* mapped = nullptr;
        const HRESULT mappedResult = target->resource->Map(0, &read, &mapped);
        ++m_calls.host;
        if (FAILED(mappedResult))
        {
            return std::unexpected(failed("texture readback: Map", mappedResult));
        }
        // A depth slice of the footprint is `rows` rows of RowPitch bytes; the result keeps `rowSize` bytes of each row.
        const auto* placed = static_cast<const std::byte*>(mapped) + footprint.Offset;
        const uint64_t rowPitch = footprint.Footprint.RowPitch;
        std::vector<std::byte> bytes(static_cast<size_t>(rowSize * rows * footprint.Footprint.Depth));
        std::byte* out = bytes.data();
        for (uint64_t slice = 0; slice < footprint.Footprint.Depth; ++slice)
        {
            for (uint64_t row = 0; row < rows; ++row)
            {
                std::memcpy(out, placed + (slice * rows + row) * rowPitch, static_cast<size_t>(rowSize));
                out += rowSize;
            }
        }
        const D3D12_RANGE written{ 0, 0 };
        target->resource->Unmap(0, &written);
        ++m_calls.host;
        return bytes;
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeContext::writeTexture(
        ID3D12Resource* image, uint32_t subresource, std::span<const std::byte> bytes, D3D12_BARRIER_LAYOUT before, D3D12_BARRIER_LAYOUT after
    )
    {
        const D3D12_RESOURCE_DESC desc = image->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 rowSize = 0;
        UINT64 total = 0;
        m_device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &rows, &rowSize, &total);
        ++m_calls.host;
        if (total == UINT64_MAX)
        {
            return std::unexpected(std::format("texture upload: the texture has no subresource {}", subresource));
        }
        const uint64_t tight = rowSize * rows * footprint.Footprint.Depth;
        if (bytes.size() != tight)
        {
            return std::unexpected(std::format("texture upload: subresource {} holds {} tight bytes, not {}", subresource, tight, bytes.size()));
        }
        const std::expected<NativeBuffer, std::string> staging = uploadBuffer(total, "bench.texture.staging");
        if (!staging)
        {
            return std::unexpected(staging.error());
        }
        const uint64_t rowPitch = footprint.Footprint.RowPitch;
        for (uint64_t row = 0; row < uint64_t{ rows } * footprint.Footprint.Depth; ++row)
        {
            std::memcpy(staging->data + footprint.Offset + row * rowPitch, bytes.data() + row * rowSize, static_cast<size_t>(rowSize));
        }
        return runSetup([&](ID3D12GraphicsCommandList9& list) {
            uint64_t barriers = 0;
            if (before != D3D12_BARRIER_LAYOUT_COPY_DEST)
            {
                recordBarrier(list, intoCopy(image, subresource, before, D3D12_BARRIER_LAYOUT_COPY_DEST));
                ++barriers;
            }
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = image;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = subresource;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = staging->resource;
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprint;
            list.CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            if (after != D3D12_BARRIER_LAYOUT_COPY_DEST)
            {
                recordBarrier(list, outOfCopy(image, subresource, D3D12_BARRIER_LAYOUT_COPY_DEST, after));
                ++barriers;
            }
            m_calls.record += 1 + barriers;
            m_calls.barriers += barriers;
        });
    }
} // namespace rojoRHI::bench
