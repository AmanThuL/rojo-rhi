//----------------------------------------------------------------------------------------------------------------------
/// @file TrackerLog.cpp
/// @brief Implements the recording hooks that append a command list's uses and barriers to its tracker log.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Tracker/TrackerLog.h"

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Formats.h"

#include <variant>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        /// Returns true when the depth-stencil attachment `depth`, of `format`, writes: its depth or stencil load clears or
        /// discards, or its depth or stencil store discards. The stencil operations count only for a format with stencil,
        /// as chapter 07 ignores them otherwise. Whether a draw writes is the draw's to add.
        [[maybe_unused]] bool depthWrites(const DepthAttachment& depth, Format format)
        {
            const bool depthWrite = depth.load != LoadOp::Load || depth.store == StoreOp::DontCare;
            const bool stencilWrite = hasStencilPlane(format) && (depth.stencilLoad != LoadOp::Load || depth.stencilStore == StoreOp::DontCare);
            return depthWrite || stencilWrite;
        }

        //==============================================================================================================
        /// Returns the single subresource at `mip` and `layer`.
        [[maybe_unused]] SubresourceRange subresource(uint32_t mip, uint32_t layer)
        {
            return SubresourceRange{ .baseMip = mip, .mipCount = 1, .baseLayer = layer, .layerCount = 1 };
        }

        //==============================================================================================================
        /// Appends `use` to the log of `list` as an attachment use of the open pass.
        [[maybe_unused]] void logAttachment(CommandListCore& list, const UseEvent& use)
        {
            list.trackerLog.passUses.push_back(static_cast<uint32_t>(list.trackerLog.events.size()));
            logUseChecked(list, use);
        }
    } // namespace

    //==================================================================================================================
    void logUseChecked([[maybe_unused]] CommandListCore& list, [[maybe_unused]] const UseEvent& use)
    {
#ifndef NDEBUG
        if (!list.device->validation() || (use.buffer.bits != 0 && use.size == 0))
        {
            return;
        }
        list.trackerLog.events.emplace_back(use);
#endif
    }

    //==================================================================================================================
    void logBarriersChecked(
        [[maybe_unused]] CommandListCore& list,
        [[maybe_unused]] std::span<const GlobalBarrier> globals,
        [[maybe_unused]] std::span<const BufferBarrier> buffers,
        [[maybe_unused]] std::span<const TextureBarrier> textures
    )
    {
#ifndef NDEBUG
        if (!list.device->validation())
        {
            return;
        }
        std::vector<TrackerEvent>& events = list.trackerLog.events;
        events.insert(events.end(), globals.begin(), globals.end());
        events.insert(events.end(), buffers.begin(), buffers.end());
        events.insert(events.end(), textures.begin(), textures.end());
#endif
    }

    //==================================================================================================================
    void logPassChecked([[maybe_unused]] CommandListCore& list, [[maybe_unused]] const RenderPassDesc& desc)
    {
#ifndef NDEBUG
        if (!list.device->validation())
        {
            return;
        }
        constexpr LayoutSet kColor = layoutSet({ Layout::ColorAttachment });
        for (const ColorAttachment& color : desc.colors)
        {
            const Access access = color.load == LoadOp::Load ? Access::ColorAttachmentRead | Access::ColorAttachmentWrite : Access::ColorAttachmentWrite;
            logAttachment(list, textureUse(color.texture, subresource(color.mip, color.layer), Stage::ColorAttachment, access, kColor, UseRule::Cmd0007));
            if (color.resolve.bits != 0)
            {
                // A resolve writes mip 0 and layer 0 of its target (chapter 07).
                logAttachment(list, textureUse(color.resolve, subresource(0, 0), Stage::Resolve, Access::Resolve, kColor, UseRule::Cmd0007));
            }
        }
        if (desc.depth.texture.bits != 0)
        {
            const Access access =
                depthWrites(desc.depth, list.pass.depthFormat) ? Access::DepthStencilRead | Access::DepthStencilWrite : Access::DepthStencilRead;
            logAttachment(
                list,
                textureUse(
                    desc.depth.texture,
                    subresource(desc.depth.mip, desc.depth.layer),
                    Stage::DepthStencil,
                    access,
                    layoutSet({ Layout::DepthStencilWrite }),
                    UseRule::Cmd0010
                )
            );
        }
#endif
    }

    //==================================================================================================================
    void logDrawChecked([[maybe_unused]] CommandListCore& list)
    {
#ifndef NDEBUG
        if (!list.device->validation() || !list.pipeline.bound)
        {
            return;
        }
        uint32_t color = 0;
        for (const uint32_t position : list.trackerLog.passUses)
        {
            UseEvent& use = std::get<UseEvent>(list.trackerLog.events[position]);
            if (use.stage == Stage::ColorAttachment)
            {
                // The pipeline's color target `color` matches the attachment `color` (chapter 06); a resolve target is no attachment.
                if ((list.pipeline.blendMask >> color & 1) != 0)
                {
                    use.access = use.access | Access::ColorAttachmentRead;
                }
                ++color;
            }
            else if (use.stage == Stage::DepthStencil && (list.pipeline.depthWrite || list.pipeline.stencilWrite))
            {
                use.access = use.access | Access::DepthStencilWrite;
            }
        }
#endif
    }

    //==================================================================================================================
    void endPassLogChecked([[maybe_unused]] CommandListCore& list)
    {
#ifndef NDEBUG
        list.trackerLog.passUses.clear();
#endif
    }
} // namespace rojoRHI::core
