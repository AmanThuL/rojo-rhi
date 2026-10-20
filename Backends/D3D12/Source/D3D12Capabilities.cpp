//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Capabilities.cpp
/// @brief Implements the D3D12 hardware floor check; the capability report is a stub that reports nothing yet.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Capabilities.h"

#include "D3D12Common.h"

#include <cstdint>
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
        // The runtime refuses a shader model newer than it knows with E_INVALIDARG, so the request steps down until it is
        // answered or reaches the floor.
        D3D12_FEATURE_DATA_SHADER_MODEL model = { D3D_HIGHEST_SHADER_MODEL };
        HRESULT result = query(device, D3D12_FEATURE_SHADER_MODEL, model);
        while (result == E_INVALIDARG && model.HighestShaderModel > D3D_SHADER_MODEL_6_6)
        {
            model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(static_cast<int>(model.HighestShaderModel) - 1);
            result = query(device, D3D12_FEATURE_SHADER_MODEL, model);
        }
        if (FAILED(result))
        {
            return belowFloor(std::format("Shader Model 6.6 (D3D12_FEATURE_SHADER_MODEL failed with HRESULT 0x{:08X})", static_cast<uint32_t>(result)));
        }
        if (model.HighestShaderModel < D3D_SHADER_MODEL_6_6)
        {
            return belowFloor(std::format("Shader Model 6.6 (the device reports {})", shaderModelName(model.HighestShaderModel)));
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        if (const HRESULT queried = query(device, D3D12_FEATURE_D3D12_OPTIONS, options); FAILED(queried))
        {
            return belowFloor(
                std::format("resource binding tier 3 (D3D12_FEATURE_D3D12_OPTIONS failed with HRESULT 0x{:08X})", static_cast<uint32_t>(queried))
            );
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
    Result<void> reportCapabilities(DeviceImpl&)
    {
        return {};
    }
} // namespace rojoRHI::d3d12
