//----------------------------------------------------------------------------------------------------------------------
/// @file Pipelines.cpp
/// @brief Implements the checks of the shader library and pipeline rules.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Pipelines.h"

#include "Base/Assert.h"
#include "Core/Formats.h"
#include "Core/Label.h"

#include <algorithm>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        /// Checks that the stage `entry` fills, if present, names an entry point of its live library with the stage `stage`
        /// (VU-rojo-pipeline-0001, VU-rojo-pipeline-0002), and returns the stage's bit, or zero for an absent stage.
        uint32_t checkEntry(DeviceCore& core, const ShaderEntry& entry, ShaderStage stage)
        {
            if (entry.library.bits == 0)
            {
                return 0;
            }
            [[maybe_unused]] const ShaderLibraryRecord* library = core.pool<ShaderLibrary>().checked(entry.library, false);
            ROJORHI_VU(!entry.name.empty(), "VU-rojo-pipeline-0001", "the {} stage names an empty entry point", stageName(stage));
            ROJORHI_VU(
                std::ranges::any_of(library->entries, [&](const ShaderEntryRecord& e) { return e.stage == stage && e.name == entry.name; }),
                "VU-rojo-pipeline-0002",
                "the library \"{}\" holds no {} entry point named \"{}\"",
                library->label,
                stageName(stage),
                entry.name
            );
            return stageBit(stage);
        }

        //==============================================================================================================
        /// Checks the color targets: their count (VU-rojo-pipeline-0010), and the format rules VU-rojo-format-0002 and 0003 for
        /// attachment, blend and sample-count support.
        void checkColors([[maybe_unused]] const DeviceCore& core, [[maybe_unused]] std::span<const ColorTarget> colors, [[maybe_unused]] uint32_t samples)
        {
            ROJORHI_VU(
                colors.size() <= core.capabilities().limits.maxColorAttachments,
                "VU-rojo-pipeline-0010",
                "{} color targets exceed the limit of {}",
                colors.size(),
                core.capabilities().limits.maxColorAttachments
            );
            for ([[maybe_unused]] const ColorTarget& target : colors)
            {
                ROJORHI_VU(target.format != Format::Undefined, "VU-rojo-format-0002", "a color target has Format::Undefined");
                [[maybe_unused]] const FormatCapabilities capabilities = core.formatCapabilities(target.format);
                ROJORHI_VU(capabilities.colorAttachment, "VU-rojo-format-0003", "the device cannot use the color target format as an attachment");
                ROJORHI_VU(
                    !target.blend.enable || capabilities.blend, "VU-rojo-format-0003", "blending is enabled for a color target format that cannot blend"
                );
                ROJORHI_VU(samplesSupported(samples, capabilities), "VU-rojo-format-0003", "a color target format does not support {} samples", samples);
            }
        }

        //==============================================================================================================
        /// Checks the depth attachment format against the depth-stencil state (VU-rojo-pipeline-0007, 0011) and the format rule
        /// VU-rojo-format-0003 for attachment and sample-count support.
        void checkDepth([[maybe_unused]] const DeviceCore& core, [[maybe_unused]] const GraphicsPipelineDesc& desc)
        {
            ROJORHI_VU(!desc.depthStencil.depthWrite || desc.depthStencil.depthTest, "VU-rojo-pipeline-0007", "depthWrite is set without depthTest");
            ROJORHI_VU(
                desc.depthFormat == Format::Undefined || isDepthFormat(desc.depthFormat),
                "VU-rojo-pipeline-0011",
                "the depth format is not Undefined or a depth format"
            );
            ROJORHI_VU(
                !desc.depthStencil.depthTest || isDepthFormat(desc.depthFormat), "VU-rojo-pipeline-0011", "depthTest is set but depthFormat has no depth plane"
            );
            ROJORHI_VU(
                !desc.depthStencil.stencilTest || hasStencilPlane(desc.depthFormat),
                "VU-rojo-pipeline-0011",
                "stencilTest is set but depthFormat has no stencil plane"
            );
            if (desc.depthFormat == Format::Undefined)
            {
                return;
            }
            [[maybe_unused]] const FormatCapabilities capabilities = core.formatCapabilities(desc.depthFormat);
            ROJORHI_VU(capabilities.depthAttachment, "VU-rojo-format-0003", "the device cannot use the depth format as a depth attachment");
            ROJORHI_VU(samplesSupported(desc.samples, capabilities), "VU-rojo-format-0003", "the depth format does not support {} samples", desc.samples);
        }
    } // namespace

    //==================================================================================================================
    std::string_view stageName(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex: return "vertex";
        case ShaderStage::Fragment: return "fragment";
        case ShaderStage::Task: return "task";
        case ShaderStage::Mesh: return "mesh";
        case ShaderStage::Compute: return "compute";
        }
        return "unknown";
    }

    //==================================================================================================================
    ShaderLibraryRecord prepareShaderLibrary([[maybe_unused]] std::span<const std::byte> blob, std::string_view label)
    {
        ROJORHI_VU(!blob.empty(), "VU-rojo-pipeline-0003", "loadShaderLibrary was given an empty blob");
        ShaderLibraryRecord record;
        record.label = resolveLabel(label, "rojorhi.shaderlibrary.unnamed");
        return record;
    }

    //==================================================================================================================
    PipelineRecord prepareGraphicsPipeline(DeviceCore& core, const GraphicsPipelineDesc& desc)
    {
        const ScopedNanoseconds timer(core.counts().pipelineCompileNanoseconds);
        PipelineRecord record;
        record.label = resolveLabel(desc.label, "rojorhi.pipeline.unnamed");
        record.kind = PipelineKind::Graphics;
        ROJORHI_VU(
            (desc.vertex.library.bits != 0) != (desc.mesh.library.bits != 0),
            "VU-rojo-pipeline-0008",
            "a graphics pipeline needs exactly one of vertex and mesh"
        );
        ROJORHI_VU(desc.task.library.bits == 0 || desc.mesh.library.bits != 0, "VU-rojo-pipeline-0009", "a task entry is present without a mesh entry");
        record.stages |= checkEntry(core, desc.vertex, ShaderStage::Vertex);
        record.stages |= checkEntry(core, desc.task, ShaderStage::Task);
        record.stages |= checkEntry(core, desc.mesh, ShaderStage::Mesh);
        record.stages |= checkEntry(core, desc.fragment, ShaderStage::Fragment);
        ROJORHI_VU(
            !desc.raster.conservative || core.capabilities().conservativeRaster,
            "VU-rojo-pipeline-0006",
            "conservative rasterization is requested but the device reports none"
        );
        checkColors(core, desc.colors, desc.samples);
        checkDepth(core, desc);
        record.primitive = desc.primitive;
        record.colors.reserve(desc.colors.size());
        for (const ColorTarget& target : desc.colors)
        {
            record.blendMask |= target.blend.enable ? uint32_t{ 1 } << record.colors.size() : 0;
            record.colors.push_back(target.format);
        }
        const DepthStencilState& stencil = desc.depthStencil;
        record.depthWrite = stencil.depthWrite;
        // Stencil operations do nothing with the stencil test off (chapter 07), and change no bit with a zero write mask, so
        // only a tested stencil with a write mask can write.
        record.stencilWrite = stencil.stencilTest && stencil.stencilWriteMask != 0 &&
                              (stencil.front.fail != StencilOp::Keep || stencil.front.depthFail != StencilOp::Keep || stencil.front.pass != StencilOp::Keep ||
                               stencil.back.fail != StencilOp::Keep || stencil.back.depthFail != StencilOp::Keep || stencil.back.pass != StencilOp::Keep);
        record.depthFormat = desc.depthFormat;
        record.samples = desc.samples;
        record.depthBias = desc.raster.depthBias;
        return record;
    }

    //==================================================================================================================
    PipelineRecord prepareComputePipeline(DeviceCore& core, const ComputePipelineDesc& desc)
    {
        const ScopedNanoseconds timer(core.counts().pipelineCompileNanoseconds);
        PipelineRecord record;
        record.label = resolveLabel(desc.label, "rojorhi.pipeline.unnamed");
        record.kind = PipelineKind::Compute;
        ROJORHI_VU(desc.compute.library.bits != 0, "VU-rojo-pipeline-0013", "a compute pipeline has no compute entry");
        record.stages = checkEntry(core, desc.compute, ShaderStage::Compute);
        return record;
    }
} // namespace rojoRHI::core
