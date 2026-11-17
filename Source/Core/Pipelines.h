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
    // Equals `ROJORHI_SHADER_ABI_VERSION` in `Shaders/RojoRHI/RojoRHI.slang` (chapter 05), which the shader build stamps into
    // every library; a change to one changes the other.
    inline constexpr uint32_t kShaderAbiVersion = 3;

    constexpr uint32_t stageBit(ShaderStage stage)
    {
        return 1u << static_cast<uint32_t>(stage);
    }

    // As a library blob spells it: vertex, fragment, task, mesh or compute.
    std::string_view stageName(ShaderStage stage);

    // The backend then parses the blob into the record's entries.
    ShaderLibraryRecord prepareShaderLibrary(std::span<const std::byte> blob, std::string_view label);

    PipelineRecord prepareGraphicsPipeline(DeviceCore& core, const GraphicsPipelineDesc& desc);

    PipelineRecord prepareComputePipeline(DeviceCore& core, const ComputePipelineDesc& desc);
} // namespace rojoRHI::core
