//----------------------------------------------------------------------------------------------------------------------
/// @file ComputePipelineDesc.h
/// @brief Declares the description of a compute pipeline.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/ShaderEntry.h>

#include <string_view>

namespace rojoRHI {
    /// Describes a compute pipeline, for `Device::createPipeline`. The threadgroup size comes from the entry's
    /// `[numthreads]` attribute. `compute` is present (VU-rojo-pipeline-0013).
    struct ComputePipelineDesc
    {
        /// The compute stage.
        ShaderEntry compute;
        /// Empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };
} // namespace rojoRHI
