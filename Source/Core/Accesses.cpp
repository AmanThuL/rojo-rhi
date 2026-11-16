//----------------------------------------------------------------------------------------------------------------------
/// @file Accesses.cpp
/// @brief Implements the access declarations of chapter 14 that a command list keeps for the validation tracker.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Accesses.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/Tracker/TrackerLog.h"

#include <bit>
#include <cstdint>

namespace rojoRHI::core {
    namespace {
        /// The bits of the shader stages a declaration may name.
        [[maybe_unused]] constexpr uint32_t kGraphicsStages =
            static_cast<uint32_t>(Stage::VertexShader | Stage::TaskShader | Stage::MeshShader | Stage::FragmentShader);

        /// The bits of the accesses a declaration may hold.
        [[maybe_unused]] constexpr uint32_t kShaderAccesses = static_cast<uint32_t>(Access::ShaderRead | Access::ShaderWrite);

        //==============================================================================================================
        /// Returns true when `stage` is exactly one of the five shader stages.
        [[maybe_unused]] bool isShaderStage(Stage stage)
        {
            const uint32_t bits = static_cast<uint32_t>(stage);
            return std::has_single_bit(bits) && (bits & (kGraphicsStages | static_cast<uint32_t>(Stage::ComputeShader))) != 0;
        }

        //==============================================================================================================
        /// Returns true when `access` holds only shader accesses and at least one.
        [[maybe_unused]] bool isShaderAccess(Access access)
        {
            const uint32_t bits = static_cast<uint32_t>(access);
            return bits != 0 && (bits & ~kShaderAccesses) == 0;
        }

        //==============================================================================================================
        /// Returns true when `access` writes.
        [[maybe_unused]] bool writes(Access access)
        {
            return (access & Access::ShaderWrite) != Access::None;
        }

        //==============================================================================================================
        /// Returns the use a declared access makes at the draw or dispatch that consumes it: a write needs `ShaderWrite`, and a
        /// read, which a declaration cannot say is through a `Sampled` or a `Storage` view, any layout chapter 09 permits either in.
        [[maybe_unused]] UseEvent declaredUse(const ResourceAccess& access)
        {
            if (access.buffer.bits != 0)
            {
                return bufferUse(access.buffer, access.offset, access.size, access.stage, access.access);
            }
            const LayoutSet layouts = writes(access.access) ? layoutSet({ Layout::ShaderWrite })
                                                            : layoutSet({ Layout::ShaderRead, Layout::DepthStencilRead, Layout::General, Layout::ShaderWrite });
            return textureUse(access.texture, access.range, access.stage, access.access, layouts, UseRule::Sync0004);
        }

        //==============================================================================================================
        /// Returns true when the run is not empty (VU-rojo-texture-0016's convention), `start` is below `limit` and `start`
        /// plus `count`, unless `all`, is at most it.
        [[maybe_unused]] bool inside(uint32_t start, uint32_t count, uint32_t all, uint32_t limit)
        {
            return count != 0 && start < limit && (count == all || uint64_t{ start } + count <= limit);
        }
    } // namespace

    //==================================================================================================================
    void declareAccess([[maybe_unused]] CommandListCore& list, [[maybe_unused]] std::span<const ResourceAccess> accesses)
    {
#ifndef NDEBUG
        DeviceCore& core = *list.device;
        if (!core.validation())
        {
            return;
        }
        ROJORHI_VU(
            list.state == ListState::Recording, "VU-rojo-diag-0004", "declareAccess was recorded on the list \"{}\", which is not recording", list.label
        );
        for (const ResourceAccess& access : accesses)
        {
            const bool hasBuffer = access.buffer.bits != 0;
            const bool hasTexture = access.texture.bits != 0;
            ROJORHI_VU(hasBuffer != hasTexture, "VU-rojo-diag-0001", "a declared access names {} buffer and {} texture", hasBuffer ? 1 : 0, hasTexture ? 1 : 0);
            ROJORHI_VU(
                isShaderStage(access.stage),
                "VU-rojo-diag-0003",
                "the declared stage {:#x} is not exactly one of the five shader stages",
                static_cast<uint32_t>(access.stage)
            );
            ROJORHI_VU(
                isShaderAccess(access.access),
                "VU-rojo-diag-0003",
                "the declared access {:#x} is not ShaderRead, ShaderWrite or both",
                static_cast<uint32_t>(access.access)
            );
            if (hasBuffer)
            {
                const BufferRecord* buffer = core.pool<Buffer>().checked(access.buffer, false);
                ROJORHI_VU(
                    access.offset < buffer->size && (access.size == kWholeSize || access.size <= buffer->size - access.offset),
                    "VU-rojo-diag-0002",
                    "the declared range of {} bytes at offset {} lies outside the buffer \"{}\" of {} bytes",
                    access.size,
                    access.offset,
                    buffer->label,
                    buffer->size
                );
                ROJORHI_VU(
                    !writes(access.access) || (buffer->usage & BufferUsage::ShaderWrite) != BufferUsage::None,
                    "VU-rojo-diag-0005",
                    "a write was declared on the buffer \"{}\", which was not created with ShaderWrite",
                    buffer->label
                );
            }
            else
            {
                const TextureRecord* texture = core.pool<Texture>().checked(access.texture, false);
                ROJORHI_VU(
                    inside(access.range.baseMip, access.range.mipCount, kAllMips, texture->mips) &&
                        inside(access.range.baseLayer, access.range.layerCount, kAllLayers, texture->layers),
                    "VU-rojo-diag-0002",
                    "the declared mips {} + {} and layers {} + {} lie outside the texture \"{}\" of {} mips and {} layers",
                    access.range.baseMip,
                    access.range.mipCount,
                    access.range.baseLayer,
                    access.range.layerCount,
                    texture->label,
                    texture->mips,
                    texture->layers
                );
                const TextureUsage needed = writes(access.access) ? TextureUsage::Storage : TextureUsage::Sampled | TextureUsage::Storage;
                ROJORHI_VU(
                    (texture->usage & needed) != TextureUsage::None,
                    "VU-rojo-diag-0005",
                    "a {} was declared on the texture \"{}\", which was not created with {}",
                    writes(access.access) ? "write" : "read",
                    texture->label,
                    writes(access.access) ? "Storage" : "Sampled or Storage"
                );
            }
            list.pendingAccesses.push_back(access);
        }
#endif
    }

    //==================================================================================================================
    void consumeAccessesChecked([[maybe_unused]] CommandListCore& list, [[maybe_unused]] bool dispatch, [[maybe_unused]] bool work)
    {
#ifndef NDEBUG
        for (const ResourceAccess& access : list.pendingAccesses)
        {
            const bool runs = dispatch ? access.stage == Stage::ComputeShader : (static_cast<uint32_t>(access.stage) & kGraphicsStages) != 0;
            ROJORHI_VU(
                runs,
                "VU-rojo-diag-0006",
                "the declared stage {:#x} is not one the next {} runs",
                static_cast<uint32_t>(access.stage),
                dispatch ? "dispatch" : "draw"
            );
            if (work)
            {
                logUse(list, declaredUse(access));
            }
        }
        list.pendingAccesses.clear();
#endif
    }

    //==================================================================================================================
    void checkNoPendingAccessesChecked([[maybe_unused]] const CommandListCore& list)
    {
        ROJORHI_VU(
            list.pendingAccesses.empty(),
            "VU-rojo-diag-0007",
            "end was called on the list \"{}\" with {} declared accesses no draw or dispatch followed",
            list.label,
            list.pendingAccesses.size()
        );
    }
} // namespace rojoRHI::core
