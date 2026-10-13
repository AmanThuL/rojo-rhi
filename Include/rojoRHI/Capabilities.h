//----------------------------------------------------------------------------------------------------------------------
/// @file Capabilities.h
/// @brief Declares the capabilities a device reports.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Limits.h>
#include <rojoRHI/UpscalerKind.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace rojoRHI {
    /// What a device supports, fixed for its lifetime. Every string and span points at device-owned storage valid until
    /// `destroyDevice`. Each `bool` gates the entries it names: using a gated feature on a device that reports `false` is a
    /// valid-usage violation of the owning chapter.
    struct Capabilities
    {
        std::string_view adapterName;            ///< The GPU's name as the native API reports it.
        std::string_view floorName;              ///< The floor met, e.g. "Apple9" or "FL 12_2, SM 6.8".
        uint32_t maxViews = 0;                   ///< Shader-visible texture views a shader can index.
        uint32_t maxSamplers = 0;                ///< Shader-visible samplers a shader can index.
        bool uploadInDeviceMemory = false;       ///< Memory::Upload is device-local and CPU-writable.
        bool placementSparse = false;            ///< BufferUsage::Sparse and TextureUsage::Sparse work.
        bool meshTasksIndirect = false;          ///< CommandList::drawMeshTasksIndirect works.
        bool conservativeRaster = false;         ///< RasterState conservative rasterization works.
        std::span<const UpscalerKind> upscalers; ///< Upscaler kinds Device::createUpscaler accepts.
        Limits limits;                           ///< Numeric limits.
    };
} // namespace rojoRHI
