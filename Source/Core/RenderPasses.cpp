//----------------------------------------------------------------------------------------------------------------------
/// @file RenderPasses.cpp
/// @brief Implements the checks of the render pass, scope, pipeline binding and dynamic state rules.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/RenderPasses.h"

#include "Base/Assert.h"
#include "Core/Formats.h"
#include "Core/Pipelines.h"
#include "Core/Scopes.h"
#include "Core/Textures.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        [[maybe_unused]] bool hasUsage(TextureUsage usage, TextureUsage flag)
        {
            return (usage & flag) != TextureUsage::None;
        }

        //==============================================================================================================
        /// The width and height of mip `mip` of `texture`.
        [[maybe_unused]] Extent2D extentAt(const TextureRecord& texture, uint32_t mip)
        {
            const Extent3D extent = mipExtent(texture.extent, mip);
            return Extent2D{ extent.width, extent.height };
        }

        //==============================================================================================================
        [[maybe_unused]] bool sameExtent(Extent2D a, Extent2D b)
        {
            return a.width == b.width && a.height == b.height;
        }

        /// One subresource a pass names, to find a subresource named twice.
        struct Subresource
        {
            uint64_t texture; ///< The texture handle's bits.
            uint32_t mip;     ///< The mip.
            uint32_t layer;   ///< The array layer.

            /// Compares two subresources.
            friend bool operator==(const Subresource&, const Subresource&) = default;
        };

        /// What `beginPass` learns of the attachments while it checks them, in Debug builds only.
        struct AttachmentSummary
        {
            std::vector<Subresource> subresources; ///< Every attachment and resolve target.
            Extent2D extent;                       ///< The extent of the first attachment at its mip.
            uint32_t samples = 0;                  ///< The sample count of the first attachment that is not a resolve target.
            bool uniformExtent = true;             ///< Every attachment has `extent`.
            bool uniformSamples = true;            ///< Every attachment that is not a resolve target has `samples`.
            uint32_t colorBytes = 0;               ///< Bytes per sample of the color formats.

            //==========================================================================================================
            /// Notes an attachment of extent `size` and, unless it resolves, of `count` samples.
            void note(Extent2D size, uint32_t count, bool resolveTarget)
            {
                if (subresources.empty())
                {
                    extent = size;
                }
                uniformExtent = uniformExtent && sameExtent(extent, size);
                if (!resolveTarget)
                {
                    samples = samples == 0 ? count : samples;
                    uniformSamples = uniformSamples && samples == count;
                }
            }
        };

        //==============================================================================================================
        /// Checks one color attachment and notes it (VU-rojo-cmd-0005, VU-rojo-cmd-0071, VU-rojo-format-0003); returns its format.
        [[maybe_unused]] Format checkColor(DeviceCore& core, const ColorAttachment& attachment, AttachmentSummary& summary)
        {
            const TextureRecord* texture = core.pool<Texture>().checked(attachment.texture, false);
            ROJORHI_VU(
                hasUsage(texture->usage, TextureUsage::RenderTarget) && texture->type != TextureType::Tex3D && attachment.mip < texture->mips &&
                    attachment.layer < texture->layers,
                "VU-rojo-cmd-0005",
                "a color attachment is not a subresource of a RenderTarget texture that is not 3D (mip {}, layer {})",
                attachment.mip,
                attachment.layer
            );
            summary.note(extentAt(*texture, attachment.mip), texture->samples, false);
            summary.subresources.push_back({ attachment.texture.bits, attachment.mip, attachment.layer });
            summary.colorBytes += formatLayout(texture->format).blockBytes;
            if (attachment.resolve.bits == 0)
            {
                return texture->format;
            }
            const TextureRecord* resolve = core.pool<Texture>().checked(attachment.resolve, false);
            ROJORHI_VU(
                texture->samples > 1 && resolve->samples == 1 && hasUsage(resolve->usage, TextureUsage::RenderTarget) && resolve->format == texture->format &&
                    sameExtent(extentAt(*resolve, 0), extentAt(*texture, attachment.mip)),
                "VU-rojo-cmd-0071",
                "a resolve target must be a single-sample RenderTarget texture of the attachment's format and extent, and the attachment must be multisampled"
            );
            ROJORHI_VU(core.formatCapabilities(resolve->format).resolve, "VU-rojo-format-0003", "the device cannot resolve the format of the resolve target");
            summary.note(extentAt(*resolve, 0), resolve->samples, true);
            summary.subresources.push_back({ attachment.resolve.bits, 0, 0 });
            return texture->format;
        }

        //==============================================================================================================
        /// Checks the depth attachment and notes it (VU-rojo-cmd-0008, VU-rojo-cmd-0009); returns its format.
        [[maybe_unused]] Format checkDepth(DeviceCore& core, const DepthAttachment& attachment, AttachmentSummary& summary)
        {
            const TextureRecord* texture = core.pool<Texture>().checked(attachment.texture, false);
            ROJORHI_VU(
                hasUsage(texture->usage, TextureUsage::DepthStencil) && isDepthFormat(texture->format) && attachment.mip < texture->mips &&
                    attachment.layer < texture->layers,
                "VU-rojo-cmd-0008",
                "the depth attachment is not a subresource of a DepthStencil texture of a depth format (mip {}, layer {})",
                attachment.mip,
                attachment.layer
            );
            ROJORHI_VU(
                attachment.clearDepth >= 0.0f && attachment.clearDepth <= 1.0f, "VU-rojo-cmd-0009", "clearDepth {} is outside [0, 1]", attachment.clearDepth
            );
            summary.note(extentAt(*texture, attachment.mip), texture->samples, false);
            summary.subresources.push_back({ attachment.texture.bits, attachment.mip, attachment.layer });
            return texture->format;
        }

        //==============================================================================================================
        /// Returns true when no subresource appears twice.
        [[maybe_unused]] bool allDistinct(const std::vector<Subresource>& subresources)
        {
            for (size_t i = 0; i < subresources.size(); ++i)
            {
                if (std::find(subresources.begin() + static_cast<std::ptrdiff_t>(i) + 1, subresources.end(), subresources[i]) != subresources.end())
                {
                    return false;
                }
            }
            return true;
        }

        //==============================================================================================================
        /// Returns what differs between the pass's attachments and the pipeline's, or an empty string when they agree.
        [[maybe_unused]] std::string_view passMismatch(const PassState& pass, const PipelineRecord& pipeline)
        {
            if (pipeline.colors != pass.colors)
            {
                return "the color formats differ";
            }
            if (pipeline.depthFormat != pass.depthFormat)
            {
                return "the depth format differs";
            }
            if (pipeline.samples != pass.samples)
            {
                return "the sample counts differ";
            }
            return {};
        }

        //==============================================================================================================
        /// Returns the class a topology belongs to.
        [[maybe_unused]] PrimitiveClass classOf(PrimitiveTopology topology)
        {
            switch (topology)
            {
            case PrimitiveTopology::PointList: return PrimitiveClass::Point;
            case PrimitiveTopology::LineList:
            case PrimitiveTopology::LineStrip: return PrimitiveClass::Line;
            case PrimitiveTopology::TriangleList:
            case PrimitiveTopology::TriangleStrip: return PrimitiveClass::Triangle;
            }
            return PrimitiveClass::Triangle;
        }

        //==============================================================================================================
        /// Returns true when a graphics pipeline with a vertex stage is bound.
        [[maybe_unused]] bool vertexPipelineBound(const BoundPipeline& pipeline)
        {
            return pipeline.bound && pipeline.kind == PipelineKind::Graphics && (pipeline.stages & stageBit(ShaderStage::Vertex)) != 0;
        }
    } // namespace

    //==================================================================================================================
    void beginPass(CommandListCore& list, const RenderPassDesc& desc)
    {
        checkRecording(list);
        ROJORHI_VU(list.type == QueueType::Graphics, "VU-rojo-cmd-0016", "beginRenderPass was recorded on the non-Graphics list \"{}\"", list.label);
        ROJORHI_VU(!list.passOpen, "VU-rojo-cmd-0016", "beginRenderPass was recorded on the list \"{}\" with a render pass already open", list.label);
        DeviceCore& core = *list.device;
        [[maybe_unused]] const Limits& limits = core.capabilities().limits;
        ROJORHI_VU(
            desc.colors.size() <= limits.maxColorAttachments,
            "VU-rojo-cmd-0011",
            "{} color attachments exceed the limit of {}",
            desc.colors.size(),
            limits.maxColorAttachments
        );
        ROJORHI_VU(!desc.colors.empty() || desc.depth.texture.bits != 0, "VU-rojo-cmd-0011", "the render pass has no attachment");
        PassState pass;
#ifndef NDEBUG
        // The attachments' records feed the Debug-tier checks and the pass state they compare against; reading them takes
        // the texture pool's lock, which Release recording never does (ADR 0019).
        AttachmentSummary summary;
        for (const ColorAttachment& attachment : desc.colors)
        {
            pass.colors.push_back(checkColor(core, attachment, summary));
        }
        if (desc.depth.texture.bits != 0)
        {
            pass.depthFormat = checkDepth(core, desc.depth, summary);
        }
        ROJORHI_VU(
            summary.uniformExtent && summary.uniformSamples,
            "VU-rojo-cmd-0012",
            "the attachments differ in extent or, other than resolve targets, in sample count"
        );
        ROJORHI_VU(
            desc.area.width > 0 && desc.area.height > 0 && uint64_t{ desc.area.x } + desc.area.width <= summary.extent.width &&
                uint64_t{ desc.area.y } + desc.area.height <= summary.extent.height,
            "VU-rojo-cmd-0013",
            "the area {}x{} at ({}, {}) is empty or exceeds the attachments' extent {}x{}",
            desc.area.width,
            desc.area.height,
            desc.area.x,
            desc.area.y,
            summary.extent.width,
            summary.extent.height
        );
        ROJORHI_VU(allDistinct(summary.subresources), "VU-rojo-cmd-0014", "a subresource appears twice among the attachments and resolve targets");
        ROJORHI_VU(
            summary.colorBytes <= limits.maxColorBytesPerSample,
            "VU-rojo-cmd-0015",
            "the color formats take {} bytes per sample, over the limit of {}",
            summary.colorBytes,
            limits.maxColorBytesPerSample
        );
        pass.samples = summary.samples;
#endif
        pass.area = desc.area;
        list.pass = std::move(pass);
        list.pipeline = {};
        recordScope(list, desc.label, "rojorhi.pass.unnamed", static_cast<uint32_t>(list.scopes.size()));
        list.passScopeBase = list.scopes.size();
        list.passOpen = true;
    }

    //==================================================================================================================
    void endPass(CommandListCore& list)
    {
        checkRecording(list);
        ROJORHI_VU(list.passOpen, "VU-rojo-cmd-0018", "endRenderPass was recorded on the list \"{}\" with no render pass open", list.label);
        ROJORHI_VU(
            list.scopes.size() == list.passScopeBase,
            "VU-rojo-cmd-0019",
            "endRenderPass was recorded with {} scopes begun inside the pass still open",
            list.scopes.size() - list.passScopeBase
        );
        list.passOpen = false;
        list.pass = {};
        list.pipeline = {};
    }

    //==================================================================================================================
    void beginScope(CommandListCore& list, std::string_view label)
    {
        checkRecording(list);
        // A scope inside a pass is a child of the pass's own scope.
        recordScope(list, label, "rojorhi.scope.unnamed", static_cast<uint32_t>(list.scopes.size() + (list.passOpen ? 1 : 0)));
        list.scopes.push_back(list.passOpen);
    }

    //==================================================================================================================
    void endScope(CommandListCore& list)
    {
        checkRecording(list);
        ROJORHI_VU(!list.scopes.empty(), "VU-rojo-cmd-0032", "endScope was recorded on the list \"{}\" with no scope open", list.label);
        ROJORHI_VU(
            list.scopes.back() == list.passOpen,
            "VU-rojo-cmd-0033",
            "endScope was recorded {} a render pass but the innermost scope began {} one",
            list.passOpen ? "inside" : "outside",
            list.scopes.back() ? "inside" : "outside"
        );
        list.scopes.pop_back();
    }

    //==================================================================================================================
    void bindPipeline(CommandListCore& list, [[maybe_unused]] Pipeline pipeline)
    {
        checkRecording(list);
#ifndef NDEBUG
        // The record feeds the Debug-tier checks and the bound state they compare against; reading it takes the pipeline
        // pool's lock, which Release recording never does (ADR 0019).
        const LookupResult<PipelineRecord> found = list.device->pool<Pipeline>().lookup(pipeline);
        ROJORHI_VU(found.status == LookupStatus::Ok, "VU-rojo-cmd-0020", "setPipeline was given handle {:#x}, which is not a live pipeline", pipeline.bits);
        const PipelineRecord& record = *found.record;
        if (record.kind == PipelineKind::Graphics)
        {
            ROJORHI_VU(list.passOpen, "VU-rojo-cmd-0021", "the graphics pipeline \"{}\" was bound outside a render pass", record.label);
            ROJORHI_VU(
                passMismatch(list.pass, record).empty(),
                "VU-rojo-cmd-0021",
                "the graphics pipeline \"{}\" does not match the render pass: {}",
                record.label,
                passMismatch(list.pass, record)
            );
        }
        else
        {
            ROJORHI_VU(
                !list.passOpen && list.type != QueueType::Copy,
                "VU-rojo-cmd-0022",
                "the compute pipeline \"{}\" was bound inside a render pass or on a Copy list",
                record.label
            );
        }
        list.pipeline = BoundPipeline{ true, record.kind, record.stages, record.primitive, record.depthBias };
#endif
    }

    //==================================================================================================================
    void setViewport(CommandListCore& list, [[maybe_unused]] const Viewport& viewport)
    {
        checkRecording(list);
        ROJORHI_VU(list.passOpen, "VU-rojo-pipeline-0015", "setViewport was recorded outside a render pass");
        ROJORHI_VU(viewport.width > 0.0f && viewport.height > 0.0f, "VU-rojo-pipeline-0016", "the viewport is {} by {}", viewport.width, viewport.height);
        ROJORHI_VU(
            viewport.minDepth >= 0.0f && viewport.minDepth <= 1.0f && viewport.maxDepth >= 0.0f && viewport.maxDepth <= 1.0f,
            "VU-rojo-pipeline-0017",
            "the viewport depth range [{}, {}] is outside [0, 1]",
            viewport.minDepth,
            viewport.maxDepth
        );
        list.pass.viewportSet = true;
    }

    //==================================================================================================================
    void setScissor(CommandListCore& list, [[maybe_unused]] const Rect2D& rect)
    {
        checkRecording(list);
        ROJORHI_VU(list.passOpen, "VU-rojo-pipeline-0018", "setScissor was recorded outside a render pass");
        [[maybe_unused]] const Rect2D& area = list.pass.area;
        ROJORHI_VU(
            rect.x >= area.x && rect.y >= area.y && uint64_t{ rect.x } + rect.width <= uint64_t{ area.x } + area.width &&
                uint64_t{ rect.y } + rect.height <= uint64_t{ area.y } + area.height,
            "VU-rojo-pipeline-0019",
            "the scissor {}x{} at ({}, {}) is outside the pass's area {}x{} at ({}, {})",
            rect.width,
            rect.height,
            rect.x,
            rect.y,
            area.width,
            area.height,
            area.x,
            area.y
        );
        list.pass.scissorSet = true;
    }

    //==================================================================================================================
    void setStencilReference(CommandListCore& list)
    {
        checkRecording(list);
        ROJORHI_VU(list.passOpen, "VU-rojo-pipeline-0020", "setStencilReference was recorded outside a render pass");
    }

    //==================================================================================================================
    void setBlendConstants(CommandListCore& list)
    {
        checkRecording(list);
        ROJORHI_VU(list.passOpen, "VU-rojo-pipeline-0021", "setBlendConstants was recorded outside a render pass");
    }

    //==================================================================================================================
    void setPrimitiveTopology(CommandListCore& list, [[maybe_unused]] PrimitiveTopology topology)
    {
        checkRecording(list);
        ROJORHI_VU(
            list.passOpen && vertexPipelineBound(list.pipeline),
            "VU-rojo-pipeline-0022",
            "setPrimitiveTopology was recorded outside a render pass or with no vertex pipeline bound"
        );
        ROJORHI_VU(
            classOf(topology) == list.pipeline.primitive,
            "VU-rojo-pipeline-0023",
            "topology {} is not of the bound pipeline's primitive class",
            static_cast<int>(topology)
        );
    }

    //==================================================================================================================
    void setDepthBias(CommandListCore& list)
    {
        checkRecording(list);
        ROJORHI_VU(list.passOpen, "VU-rojo-pipeline-0024", "setDepthBias was recorded outside a render pass");
        ROJORHI_VU(
            list.pipeline.bound && list.pipeline.kind == PipelineKind::Graphics && list.pipeline.depthBias,
            "VU-rojo-pipeline-0025",
            "setDepthBias was recorded with no graphics pipeline bound that was created with depthBias"
        );
    }
} // namespace rojoRHI::core
