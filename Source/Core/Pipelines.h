//----------------------------------------------------------------------------------------------------------------------
/// @file Pipelines.h
/// @brief Declares the checks of the shader library and pipeline rules of chapter 06, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Records.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace rojoRHI::core {
    /// The shader ABI version a library must be stamped with: the value of `ROJORHI_SHADER_ABI_VERSION` in `RojoRHI.slang`
    /// (chapter 05), which the shader build stamps into every library it builds.
    /// A change to either constant must change the other: `Shaders/RojoRHI/RojoRHI.slang` declares the same number.
    inline constexpr uint32_t kShaderAbiVersion = 2;

    /// Returns the bit `PipelineRecord::stages` holds for `stage`.
    constexpr uint32_t stageBit(ShaderStage stage)
    {
        return 1u << static_cast<uint32_t>(stage);
    }

    /// Returns the name of `stage` as a library blob and a message spell it: vertex, fragment, task, mesh or compute.
    std::string_view stageName(ShaderStage stage);

    /// Checks the arguments of `Device::loadShaderLibrary` (VU-rojo-pipeline-0003, VU-rojo-core-0006, VU-rojo-core-0007) and
    /// returns the record of a library with no entries and no native payload. A backend calls it first in `loadShaderLibrary`,
    /// then parses the blob into the record's entries.
    ShaderLibraryRecord prepareShaderLibrary(std::span<const std::byte> blob, std::string_view label);

    /// Checks a graphics pipeline description (VU-rojo-pipeline-0001, 0002, 0006 to 0011, VU-rojo-format-0002,
    /// VU-rojo-format-0003, the label rules, and the library lookup rules VU-rojo-core-0001 to 0003) against the device and
    /// the libraries its entries name, and returns the record of the pipeline it describes with no native payload. A backend
    /// calls it first in `createPipeline`.
    PipelineRecord prepareGraphicsPipeline(DeviceCore& core, const GraphicsPipelineDesc& desc);

    /// Checks a compute pipeline description (VU-rojo-pipeline-0001, 0002, 0013, the label rules and the library lookup rules)
    /// and returns the record of the pipeline it describes with no native payload. A backend calls it first in
    /// `createPipeline`.
    PipelineRecord prepareComputePipeline(DeviceCore& core, const ComputePipelineDesc& desc);
} // namespace rojoRHI::core
