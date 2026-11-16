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
    /// The optional features of a device, read once at creation, which later entries consult.
    struct DeviceFeatures
    {
        uint32_t shaderModel = 0;            ///< The highest shader model, as `D3D_SHADER_MODEL` encodes it (0x6N).
        bool gpuUploadHeap = false;          ///< `D3D12_FEATURE_D3D12_OPTIONS16::GPUUploadHeapSupported`.
        bool relaxedFormatCasting = false;   ///< `D3D12_FEATURE_D3D12_OPTIONS12::RelaxedFormatCastingSupported`.
        uint32_t tiledResourcesTier = 0;     ///< `D3D12_FEATURE_D3D12_OPTIONS::TiledResourcesTier`.
        uint32_t conservativeRasterTier = 0; ///< `D3D12_FEATURE_D3D12_OPTIONS::ConservativeRasterizationTier`.
        bool copyQueueTimestamps = false;    ///< `D3D12_FEATURE_D3D12_OPTIONS3::CopyQueueTimestampQueriesSupported`.
    };

    /// Checks the hardware floor on `device`, in order: Shader Model 6.6, resource binding tier 3,
    /// `TypedUAVLoadAdditionalFormats`, resource heap tier 2, enhanced barriers and dynamic depth bias. Returns `Unsupported`
    /// naming the first item the device misses. Feature level 12_2 is not checked here: the device was created at it.
    Result<void> checkFloor(ID3D12Device12* device);

    /// Fills `impl.features` and the capabilities, limits and format table `impl.core` reports, from `impl`'s device.
    Result<void> reportCapabilities(DeviceImpl& impl);
} // namespace rojoRHI::d3d12
