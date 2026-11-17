//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Capabilities.h
/// @brief Declares the D3D12 hardware floor check, the optional features a device reports and the capability report.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <cstdint>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
    struct DeviceFeatures
    {
        uint32_t shaderModel = 0; // As D3D_SHADER_MODEL encodes it (0x6N).
        bool gpuUploadHeap = false;
        bool relaxedFormatCasting = false;
        uint32_t tiledResourcesTier = 0;
        uint32_t conservativeRasterTier = 0;
        bool copyQueueTimestamps = false;
    };

    // Checks, in order: Shader Model 6.6, resource binding tier 3, TypedUAVLoadAdditionalFormats, resource heap tier 2,
    // enhanced barriers and dynamic depth bias, and names the first one missed. Feature level 12_2 is not checked: the
    // device was created at it.
    Result<void> checkFloor(ID3D12Device12* device);

    Result<void> reportCapabilities(DeviceImpl& impl);
} // namespace rojoRHI::d3d12
