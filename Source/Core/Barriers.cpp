//----------------------------------------------------------------------------------------------------------------------
/// @file Barriers.cpp
/// @brief Implements the checks of the barrier rules.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Barriers.h"

#include "Base/Assert.h"
#include "Core/Textures.h"
#include "Core/Tracker/TrackerLog.h"

#include <cstdint>

namespace rojoRHI::core {
    namespace {
        constexpr Stage kShaderStages = Stage::VertexShader | Stage::TaskShader | Stage::MeshShader | Stage::FragmentShader | Stage::ComputeShader;

        //==============================================================================================================
        [[maybe_unused]] bool any(Stage stages, Stage mask)
        {
            return (stages & mask) != Stage::None;
        }

        //==============================================================================================================
        [[maybe_unused]] bool any(Access accesses, Access mask)
        {
            return (accesses & mask) != Access::None;
        }

        //==============================================================================================================
        /// Returns the stages that perform `access`: the access bits of the table of chapter 09 and where each is performed.
        [[maybe_unused]] Stage stagesPerforming(Access access)
        {
            Stage stages = Stage::None;
            if (any(access, Access::IndirectArgs))
            {
                stages = stages | Stage::IndirectArgs;
            }
            if (any(access, Access::IndexRead))
            {
                stages = stages | Stage::IndexInput;
            }
            if (any(access, Access::ShaderRead | Access::ShaderWrite))
            {
                stages = stages | kShaderStages;
            }
            if (any(access, Access::ColorAttachmentRead | Access::ColorAttachmentWrite))
            {
                stages = stages | Stage::ColorAttachment;
            }
            if (any(access, Access::DepthStencilRead | Access::DepthStencilWrite))
            {
                stages = stages | Stage::DepthStencil;
            }
            if (any(access, Access::CopyRead | Access::CopyWrite))
            {
                stages = stages | Stage::Copy;
            }
            if (any(access, Access::Resolve))
            {
                stages = stages | Stage::Resolve;
            }
            return stages;
        }

        //==============================================================================================================
        /// Returns true when every access bit of `access` is performed by some stage of `stages`.
        [[maybe_unused]] bool accessesPerformed(Stage stages, Access access)
        {
            constexpr Access kBits[] = { Access::IndirectArgs,
                                         Access::IndexRead,
                                         Access::ShaderRead,
                                         Access::ShaderWrite,
                                         Access::ColorAttachmentRead,
                                         Access::ColorAttachmentWrite,
                                         Access::DepthStencilRead,
                                         Access::DepthStencilWrite,
                                         Access::CopyRead,
                                         Access::CopyWrite,
                                         Access::Resolve };
            for (const Access bit : kBits)
            {
                if (any(access, bit) && !any(stages, stagesPerforming(bit)))
                {
                    return false;
                }
            }
            return (access & ~(Access::IndirectArgs | Access::IndexRead | Access::ShaderRead | Access::ShaderWrite | Access::ColorAttachmentRead |
                               Access::ColorAttachmentWrite | Access::DepthStencilRead | Access::DepthStencilWrite | Access::CopyRead | Access::CopyWrite |
                               Access::Resolve)) == Access::None;
        }

        //==============================================================================================================
        /// Returns the stages a barrier on a list of `type` may name besides `Stage::All`.
        [[maybe_unused]] Stage stagesOf(QueueType type)
        {
            switch (type)
            {
            case QueueType::Compute: return Stage::IndirectArgs | Stage::ComputeShader | Stage::Copy;
            case QueueType::Copy: return Stage::Copy;
            default: return Stage::All;
            }
        }

        //==============================================================================================================
        /// Returns true when `stages` names only stages `type` has, or is `Stage::All`, which stands for the whole queue.
        [[maybe_unused]] bool stagesFit(QueueType type, Stage stages)
        {
            if (type == QueueType::Graphics)
            {
                return true;
            }
            return stages == Stage::All || (stages & ~stagesOf(type)) == Stage::None;
        }

        //==============================================================================================================
        /// Checks the stage and access sets of one side of a barrier of any kind.
        [[maybe_unused]] void checkSide(
            [[maybe_unused]] const CommandListCore& list, [[maybe_unused]] Stage stage, [[maybe_unused]] Access access, [[maybe_unused]] const char* side
        )
        {
            ROJORHI_VU(
                stagesFit(list.type, stage),
                "VU-rojo-sync-0001",
                "the {} stage set {:#x} of a barrier names a stage the list \"{}\" of queue type {} cannot use",
                side,
                static_cast<uint32_t>(stage),
                list.label,
                static_cast<int>(list.type)
            );
            ROJORHI_VU(
                stage != Stage::None || access == Access::None,
                "VU-rojo-sync-0003",
                "the {} stage of a barrier is None but its access set is {:#x}",
                side,
                static_cast<uint32_t>(access)
            );
            ROJORHI_VU(
                accessesPerformed(stage, access),
                "VU-rojo-sync-0002",
                "the {} access set {:#x} of a barrier is not performed by its stage set {:#x}",
                side,
                static_cast<uint32_t>(access),
                static_cast<uint32_t>(stage)
            );
        }

        //==============================================================================================================
        [[maybe_unused]] bool rangeInside(uint64_t offset, uint64_t size, uint64_t bufferSize)
        {
            return size <= bufferSize && offset <= bufferSize - size;
        }

        //==============================================================================================================
        /// Returns true when a buffer barrier names a range inside a buffer of `bufferSize` bytes.
        [[maybe_unused]] bool bufferRangeFits(const BufferBarrier& barrier, uint64_t bufferSize)
        {
            if (barrier.offset >= bufferSize)
            {
                return false;
            }
            return barrier.size == kWholeSize || (barrier.size > 0 && rangeInside(barrier.offset, barrier.size, bufferSize));
        }

        //==============================================================================================================
        /// Returns the layouts a texture of `usage` may be moved to.
        [[maybe_unused]] bool usageAllowsLayout(TextureUsage usage, Layout layout)
        {
            auto has = [usage](TextureUsage flag) { return (usage & flag) != TextureUsage::None; };
            switch (layout)
            {
            case Layout::ShaderRead: return has(TextureUsage::Sampled);
            case Layout::ShaderWrite: return has(TextureUsage::Storage);
            case Layout::ColorAttachment: return has(TextureUsage::RenderTarget);
            case Layout::DepthStencilWrite:
            case Layout::DepthStencilRead: return has(TextureUsage::DepthStencil);
            default: return true;
            }
        }

        //==============================================================================================================
        /// Returns the accesses a subresource in `layout` permits: the layout column of the table of chapter 09.
        [[maybe_unused]] Access accessesOf(Layout layout)
        {
            switch (layout)
            {
            case Layout::General: return Access::ShaderRead | Access::CopyRead | Access::CopyWrite;
            case Layout::ShaderRead:
            case Layout::DepthStencilRead: return Access::ShaderRead;
            case Layout::ShaderWrite: return Access::ShaderRead | Access::ShaderWrite;
            case Layout::ColorAttachment: return Access::ColorAttachmentRead | Access::ColorAttachmentWrite | Access::Resolve;
            case Layout::DepthStencilWrite: return Access::DepthStencilRead | Access::DepthStencilWrite;
            case Layout::CopySrc: return Access::CopyRead;
            case Layout::CopyDst: return Access::CopyWrite;
            default: return Access::None;
            }
        }

        //==============================================================================================================
        [[maybe_unused]] bool computeLayout(Layout layout)
        {
            switch (layout)
            {
            case Layout::Undefined:
            case Layout::General:
            case Layout::ShaderRead:
            case Layout::ShaderWrite:
            case Layout::CopySrc:
            case Layout::CopyDst: return true;
            default: return false;
            }
        }

        //==============================================================================================================
        /// Returns the number of mips `range` covers in a texture of `mips` mips; the range was checked to start inside.
        [[maybe_unused]] uint64_t resolved(uint32_t base, uint32_t count, uint32_t total)
        {
            return count == kAllMips ? total - base : count;
        }

        //==============================================================================================================
        /// Returns true when two ranges of a texture of `a`'s record share a mip and a layer.
        [[maybe_unused]] bool rangesShare(const TextureRecord& texture, const SubresourceRange& a, const SubresourceRange& b)
        {
            const uint64_t aMipEnd = uint64_t{ a.baseMip } + resolved(a.baseMip, a.mipCount, texture.mips);
            const uint64_t bMipEnd = uint64_t{ b.baseMip } + resolved(b.baseMip, b.mipCount, texture.mips);
            const uint64_t aLayerEnd = uint64_t{ a.baseLayer } + resolved(a.baseLayer, a.layerCount, texture.layers);
            const uint64_t bLayerEnd = uint64_t{ b.baseLayer } + resolved(b.baseLayer, b.layerCount, texture.layers);
            return a.baseMip < bMipEnd && b.baseMip < aMipEnd && a.baseLayer < bLayerEnd && b.baseLayer < aLayerEnd;
        }
    } // namespace

    //==================================================================================================================
    void barrier(
        CommandListCore& list, std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers, std::span<const TextureBarrier> textures
    )
    {
        checkRecording(list);
        if (!globals.empty() || !buffers.empty() || !textures.empty())
        {
            // The records of one call batch into one native barrier command.
            list.counts.globalBarriers += globals.size();
            list.counts.bufferBarriers += buffers.size();
            list.counts.textureBarriers += textures.size();
            noteCommand(list);
        }
        ROJORHI_VU(!list.passOpen, "VU-rojo-sync-0019", "a barrier was recorded on the list \"{}\" inside a render pass", list.label);
#ifndef NDEBUG
        // The loops only feed Debug-tier checks, and reading a record takes its pool's lock (ADR 0020).
        for (const GlobalBarrier& global : globals)
        {
            checkSide(list, global.srcStage, global.srcAccess, "source");
            checkSide(list, global.dstStage, global.dstAccess, "destination");
            ROJORHI_VU(
                global.srcStage != Stage::None || global.dstStage != Stage::None, "VU-rojo-sync-0008", "a global barrier has the stage None on both sides"
            );
        }
        for (const BufferBarrier& entry : buffers)
        {
            const BufferRecord* record = list.device->pool<Buffer>().checked(entry.buffer, false);
            ROJORHI_VU(
                bufferRangeFits(entry, record->size),
                "VU-rojo-sync-0010",
                "a buffer barrier names {} bytes from {} of the buffer \"{}\" of {} bytes",
                entry.size,
                entry.offset,
                record->label,
                record->size
            );
            checkSide(list, entry.srcStage, entry.srcAccess, "source");
            checkSide(list, entry.dstStage, entry.dstAccess, "destination");
            [[maybe_unused]] constexpr Access kBufferAccesses =
                Access::IndirectArgs | Access::IndexRead | Access::ShaderRead | Access::ShaderWrite | Access::CopyRead | Access::CopyWrite;
            ROJORHI_VU(
                ((entry.srcAccess | entry.dstAccess) & ~kBufferAccesses) == Access::None,
                "VU-rojo-sync-0011",
                "a barrier of the buffer \"{}\" names the access set {:#x}, which a buffer does not have",
                record->label,
                static_cast<uint32_t>(entry.srcAccess | entry.dstAccess)
            );
        }
        for (const TextureBarrier& entry : textures)
        {
            const TextureRecord* record = list.device->pool<Texture>().checked(entry.texture, false);
            checkSubresourceRange(*record, entry.range);
            checkSide(list, entry.srcStage, entry.srcAccess, "source");
            checkSide(list, entry.dstStage, entry.dstAccess, "destination");
            ROJORHI_VU(
                record->swapchainImage || (entry.oldLayout != Layout::Present && entry.newLayout != Layout::Present),
                "VU-rojo-sync-0005",
                "a barrier moves the texture \"{}\", which no swapchain acquired, from layout {} to {}; only a swapchain image uses Present",
                record->label,
                static_cast<int>(entry.oldLayout),
                static_cast<int>(entry.newLayout)
            );
            ROJORHI_VU(entry.newLayout != Layout::Undefined, "VU-rojo-sync-0014", "a barrier of the texture \"{}\" moves it to Undefined", record->label);
            ROJORHI_VU(
                usageAllowsLayout(record->usage, entry.newLayout),
                "VU-rojo-sync-0015",
                "the texture \"{}\" was created without the usage the layout {} needs",
                record->label,
                static_cast<int>(entry.newLayout)
            );
            ROJORHI_VU(
                entry.newLayout != Layout::DepthStencilRead || (record->usage & TextureUsage::Sampled) != TextureUsage::None,
                "VU-rojo-sync-0027",
                "a barrier moves the texture \"{}\" to DepthStencilRead, but it was created without the Sampled usage that layout needs",
                record->label
            );
            ROJORHI_VU(
                (entry.srcAccess & ~accessesOf(entry.oldLayout)) == Access::None && (entry.dstAccess & ~accessesOf(entry.newLayout)) == Access::None,
                "VU-rojo-sync-0016",
                "the accesses {:#x} and {:#x} of a barrier of the texture \"{}\" are not permitted by the layouts {} and {}",
                static_cast<uint32_t>(entry.srcAccess),
                static_cast<uint32_t>(entry.dstAccess),
                record->label,
                static_cast<int>(entry.oldLayout),
                static_cast<int>(entry.newLayout)
            );
            ROJORHI_VU(
                list.type != QueueType::Compute || (computeLayout(entry.oldLayout) && computeLayout(entry.newLayout)),
                "VU-rojo-sync-0007",
                "a barrier on the Compute list \"{}\" moves the texture \"{}\" from layout {} to {}, which a Compute queue does not accept",
                list.label,
                record->label,
                static_cast<int>(entry.oldLayout),
                static_cast<int>(entry.newLayout)
            );
            ROJORHI_VU(
                list.type != QueueType::Copy || (entry.oldLayout == Layout::General && entry.newLayout == Layout::General),
                "VU-rojo-sync-0026",
                "a barrier on the Copy list \"{}\" moves the texture \"{}\" from layout {} to {}; a Copy list keeps a texture in General on "
                "both sides",
                list.label,
                record->label,
                static_cast<int>(entry.oldLayout),
                static_cast<int>(entry.newLayout)
            );
        }
        for (size_t later = 1; later < textures.size(); ++later)
        {
            for (size_t earlier = 0; earlier < later; ++earlier)
            {
                if (textures[earlier].texture.bits != textures[later].texture.bits)
                {
                    continue;
                }
                const TextureRecord* record = list.device->pool<Texture>().checked(textures[later].texture, false);
                ROJORHI_VU(
                    !rangesShare(*record, textures[earlier].range, textures[later].range),
                    "VU-rojo-sync-0020",
                    "the texture barriers {} and {} of one batch cover the same subresource of the texture \"{}\"",
                    earlier,
                    later,
                    record->label
                );
            }
        }
#endif
        logBarriers(list, globals, buffers, textures);
    }
} // namespace rojoRHI::core
