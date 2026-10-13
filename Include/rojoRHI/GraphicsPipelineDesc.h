//----------------------------------------------------------------------------------------------------------------------
/// @file GraphicsPipelineDesc.h
/// @brief Declares the description of a graphics pipeline.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/ColorTarget.h>
#include <rojoRHI/DepthStencilState.h>
#include <rojoRHI/Format.h>
#include <rojoRHI/PrimitiveClass.h>
#include <rojoRHI/RasterState.h>
#include <rojoRHI/ShaderEntry.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace rojoRHI {
    /// Describes a vertex pipeline (`vertex`, optional `fragment`) or a mesh pipeline (optional `task`, `mesh`, optional
    /// `fragment`), for `Device::createPipeline`; spans and strings are copied. Exactly one of `vertex` and `mesh` is present
    /// (VU-rojo-pipeline-0008) and `task` only with `mesh` (VU-rojo-pipeline-0009); `colors` holds at most
    /// `Limits::maxColorAttachments` entries (VU-rojo-pipeline-0010); `depthFormat` is `Undefined` or a depth format that
    /// supplies the components the depth-stencil state tests (VU-rojo-pipeline-0011); every attachment format supports
    /// `samples` (VU-rojo-format-0003).
    struct GraphicsPipelineDesc
    {
        /// The vertex stage, or absent for a mesh pipeline.
        ShaderEntry vertex;
        /// The optional task stage of a mesh pipeline.
        ShaderEntry task;
        /// The mesh stage, or absent for a vertex pipeline.
        ShaderEntry mesh;
        /// The fragment stage; absent for depth-only pipelines.
        ShaderEntry fragment;
        /// The topology class; the exact topology is dynamic.
        PrimitiveClass primitive = PrimitiveClass::Triangle;
        /// Rasterizer state.
        RasterState raster;
        /// Depth and stencil state.
        DepthStencilState depthStencil;
        /// The color outputs; entry `i` matches color attachment `i`.
        std::span<const ColorTarget> colors;
        /// The depth attachment format; `Undefined` means no depth attachment.
        Format depthFormat = Format::Undefined;
        /// Samples per texel of every attachment.
        uint32_t samples = 1;
        /// Empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };
} // namespace rojoRHI
