//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Capabilities.cpp
/// @brief Implements the D3D12 hardware floor check and the capability, limit and format reports of a device.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Capabilities.h"

#include "Core/DeviceCore.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Device.h"
#include "D3D12Formats.h"

#include <algorithm>
#include <cstdint>
#include <dxgi1_6.h>

#include <format>
#include <string>
#include <utility>

namespace rojoRHI::d3d12 {
    namespace {
        //==============================================================================================================
        /// Returns the error of a floor item `device` misses.
        std::unexpected<Error> belowFloor(std::string item)
        {
            return std::unexpected(Error{ ErrorCategory::Unsupported, std::format("the D3D12 hardware floor is not met: {}", std::move(item)) });
        }

        //==============================================================================================================
        /// Reads `feature` of `device` into `data`.
        template<typename T> HRESULT query(ID3D12Device12* device, D3D12_FEATURE feature, T& data)
        {
            return device->CheckFeatureSupport(feature, &data, static_cast<UINT>(sizeof(T)));
        }

        //==============================================================================================================
        /// Reads the highest shader model of `device` into `model`. The runtime refuses a shader model newer than it knows with
        /// E_INVALIDARG, so the request steps down until it is answered or reaches Shader Model 6.6.
        HRESULT queryShaderModel(ID3D12Device12* device, D3D12_FEATURE_DATA_SHADER_MODEL& model)
        {
            model = { D3D_HIGHEST_SHADER_MODEL };
            HRESULT result = query(device, D3D12_FEATURE_SHADER_MODEL, model);
            while (result == E_INVALIDARG && model.HighestShaderModel > D3D_SHADER_MODEL_6_6)
            {
                model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(static_cast<int>(model.HighestShaderModel) - 1);
                result = query(device, D3D12_FEATURE_SHADER_MODEL, model);
            }
            return result;
        }

        //==============================================================================================================
        /// Returns the shader model encoded as `D3D_SHADER_MODEL` as "6.N".
        std::string shaderModelName(D3D_SHADER_MODEL model)
        {
            const int value = static_cast<int>(model);
            return std::format("{}.{}", value >> 4, value & 0xF);
        }
    } // namespace

    //==================================================================================================================
    Result<void> checkFloor(ID3D12Device12* device)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL model = {};
        const HRESULT result = queryShaderModel(device, model);
        if (FAILED(result))
        {
            return belowFloor(std::format("Shader Model 6.6 (D3D12_FEATURE_SHADER_MODEL failed with HRESULT 0x{:08X})", hresultBits(result)));
        }
        if (model.HighestShaderModel < D3D_SHADER_MODEL_6_6)
        {
            return belowFloor(std::format("Shader Model 6.6 (the device reports {})", shaderModelName(model.HighestShaderModel)));
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        if (const HRESULT queried = query(device, D3D12_FEATURE_D3D12_OPTIONS, options); FAILED(queried))
        {
            return belowFloor(std::format("resource binding tier 3 (D3D12_FEATURE_D3D12_OPTIONS failed with HRESULT 0x{:08X})", hresultBits(queried)));
        }
        if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
        {
            return belowFloor(std::format("resource binding tier 3 (the device reports tier {})", static_cast<int>(options.ResourceBindingTier)));
        }
        if (options.TypedUAVLoadAdditionalFormats == FALSE)
        {
            return belowFloor("TypedUAVLoadAdditionalFormats");
        }
        // Heaps that hold buffers and textures alike need resource heap tier 2.
        if (options.ResourceHeapTier < D3D12_RESOURCE_HEAP_TIER_2)
        {
            return belowFloor(std::format("resource heap tier 2 (the device reports tier {})", static_cast<int>(options.ResourceHeapTier)));
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
        if (const HRESULT queried = query(device, D3D12_FEATURE_D3D12_OPTIONS12, options12); FAILED(queried) || options12.EnhancedBarriersSupported == FALSE)
        {
            return belowFloor("enhanced barriers (D3D12_FEATURE_D3D12_OPTIONS12::EnhancedBarriersSupported)");
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
        if (const HRESULT queried = query(device, D3D12_FEATURE_D3D12_OPTIONS16, options16); FAILED(queried) || options16.DynamicDepthBiasSupported == FALSE)
        {
            return belowFloor("dynamic depth bias (D3D12_FEATURE_D3D12_OPTIONS16::DynamicDepthBiasSupported)");
        }
        return {};
    }

    //==================================================================================================================
    Result<void> reportCapabilities(DeviceImpl& impl)
    {
        ID3D12Device12* device = impl.device();
        D3D12_FEATURE_DATA_SHADER_MODEL model = {};
        if (const HRESULT result = queryShaderModel(device, model); FAILED(result))
        {
            return std::unexpected(failure(impl, result, "D3D12_FEATURE_SHADER_MODEL"));
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        if (const HRESULT result = query(device, D3D12_FEATURE_D3D12_OPTIONS, options); FAILED(result))
        {
            return std::unexpected(failure(impl, result, "D3D12_FEATURE_D3D12_OPTIONS"));
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3 = {};
        if (const HRESULT result = query(device, D3D12_FEATURE_D3D12_OPTIONS3, options3); FAILED(result))
        {
            return std::unexpected(failure(impl, result, "D3D12_FEATURE_D3D12_OPTIONS3"));
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
        if (const HRESULT result = query(device, D3D12_FEATURE_D3D12_OPTIONS12, options12); FAILED(result))
        {
            return std::unexpected(failure(impl, result, "D3D12_FEATURE_D3D12_OPTIONS12"));
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
        if (const HRESULT result = query(device, D3D12_FEATURE_D3D12_OPTIONS16, options16); FAILED(result))
        {
            return std::unexpected(failure(impl, result, "D3D12_FEATURE_D3D12_OPTIONS16"));
        }
        D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT address = {};
        if (const HRESULT result = query(device, D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, address); FAILED(result))
        {
            return std::unexpected(failure(impl, result, "D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT"));
        }
        DXGI_ADAPTER_DESC1 adapter = {};
        if (const HRESULT result = impl.loaded.adapter->GetDesc1(&adapter); FAILED(result))
        {
            return std::unexpected(failure(impl, result, "IDXGIAdapter1::GetDesc1"));
        }

        impl.features.shaderModel = static_cast<uint32_t>(model.HighestShaderModel);
        impl.features.gpuUploadHeap = options16.GPUUploadHeapSupported != FALSE;
        impl.features.relaxedFormatCasting = options12.RelaxedFormatCastingSupported != FALSE;
        impl.features.tiledResourcesTier = static_cast<uint32_t>(options.TiledResourcesTier);
        impl.features.conservativeRasterTier = static_cast<uint32_t>(options.ConservativeRasterizationTier);
        impl.features.copyQueueTimestamps = options3.CopyQueueTimestampQueriesSupported != FALSE;

        impl.adapterName = utf8(adapter.Description);
        impl.floorName = std::format("FL 12_2, SM {}", shaderModelName(model.HighestShaderModel));

        // Nothing past the floor is reported until the backend implements it: no placement sparse, no conservative raster, no
        // upscaler.
        Capabilities& capabilities = impl.core.capabilities();
        capabilities.adapterName = impl.adapterName;
        capabilities.floorName = impl.floorName;
        capabilities.maxViews = impl.descriptors.viewCapacity();
        capabilities.maxSamplers = impl.descriptors.samplerCapacity();
        capabilities.uploadInDeviceMemory = impl.features.gpuUploadHeap;
        capabilities.placementSparse = false;
        capabilities.meshTasksIndirect = true;
        capabilities.conservativeRaster = false;
        capabilities.upscalers = {};

        // A buffer is as large as the adapter's memory, which is shared memory on an adapter with no dedicated memory, and no
        // larger than one resource's virtual address space.
        const uint64_t memory = adapter.DedicatedVideoMemory != 0 ? adapter.DedicatedVideoMemory : adapter.SharedSystemMemory;
        const uint64_t reach = uint64_t{ 1 } << std::min<UINT>(address.MaxGPUVirtualAddressBitsPerResource, 63);

        Limits& limits = capabilities.limits;
        limits.maxViews = capabilities.maxViews;
        limits.maxSamplers = capabilities.maxSamplers;
        limits.maxRootBlockSize = 65'536;
        limits.rootAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        limits.frameDataAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        limits.maxBufferSize = std::min(memory, reach);
        limits.maxAddrOffset = kMaxAddrOffset;
        limits.maxTextureDimension1D = D3D12_REQ_TEXTURE1D_U_DIMENSION;
        limits.maxTextureDimension2D = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        limits.maxTextureDimension3D = D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
        limits.maxTextureDimensionCube = D3D12_REQ_TEXTURECUBE_DIMENSION;
        limits.maxTextureLayers = D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
        limits.maxColorAttachments = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
        // D3D12 has no per-sample byte budget below 8 targets of 16 bytes, so the floor of 64 is what binds.
        limits.maxColorBytesPerSample = 64;
        limits.maxSamplerAnisotropy = D3D12_REQ_MAXANISOTROPY;
        limits.maxDrawIndirectCount = UINT32_MAX;
        limits.maxDispatchGroups = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
        limits.maxThreadsPerGroup = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
        limits.maxGroupSize[0] = D3D12_CS_THREAD_GROUP_MAX_X;
        limits.maxGroupSize[1] = D3D12_CS_THREAD_GROUP_MAX_Y;
        limits.maxGroupSize[2] = D3D12_CS_THREAD_GROUP_MAX_Z;
        // Group-shared memory is counted in four-byte registers.
        limits.maxGroupSharedMemory = D3D12_CS_TGSM_REGISTER_COUNT * 4;
        limits.maxMeshTaskGroups = 65'535;
        limits.maxMeshTaskGroupsTotal = 4'194'304;

        impl.core.setFormatTable(queryFormatTable(device));
        return {};
    }
} // namespace rojoRHI::d3d12
