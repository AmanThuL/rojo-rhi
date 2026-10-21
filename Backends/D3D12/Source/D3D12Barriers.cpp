//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Barriers.cpp
/// @brief Implements the lowering of barrier batches to one D3D12 enhanced `Barrier` call per batch.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Barriers.h"

#include <rojoRHI/RHI.h>

#include "Core/Barriers.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Formats.h"
#include "Core/HandlePool.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12ListState.h"
#include "D3D12Textures.h"

#include <array>
#include <cstdint>
#include <span>

namespace rojoRHI {
    namespace d3d12 {
        namespace {
            //==========================================================================================================
            /// Returns true when `stages` and `mask` share a bit.
            bool any(Stage stages, Stage mask)
            {
                return (stages & mask) != Stage::None;
            }

            //==========================================================================================================
            /// Returns true when `access` and `mask` share a bit.
            bool any(Access access, Access mask)
            {
                return (access & mask) != Access::None;
            }

            /// A sync and access pair: a sync bit and the access bits compatible with it, from the access and sync
            /// compatibility of enhanced barriers, or the sync and access of one side of a barrier.
            struct SyncAccess
            {
                D3D12_BARRIER_SYNC sync;     ///< The sync scopes.
                D3D12_BARRIER_ACCESS access; ///< The access bits.
            };

            /// Every sync bit the lowering names besides `SYNC_ALL`, with the accesses it is compatible with.
            constexpr std::array<SyncAccess, 12> kSyncAccesses = { {
                { D3D12_BARRIER_SYNC_DRAW,
                  D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER | D3D12_BARRIER_ACCESS_INDEX_BUFFER |
                      D3D12_BARRIER_ACCESS_RENDER_TARGET | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE |
                      D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ | D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_STREAM_OUTPUT },
                { D3D12_BARRIER_SYNC_INDEX_INPUT, D3D12_BARRIER_ACCESS_INDEX_BUFFER },
                { D3D12_BARRIER_SYNC_VERTEX_SHADING,
                  D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS |
                      D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_STREAM_OUTPUT },
                { D3D12_BARRIER_SYNC_PIXEL_SHADING,
                  D3D12_BARRIER_ACCESS_CONSTANT_BUFFER | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE },
                { D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ },
                { D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET },
                { D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                  D3D12_BARRIER_ACCESS_CONSTANT_BUFFER | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE },
                { D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST | D3D12_BARRIER_ACCESS_COPY_SOURCE },
                { D3D12_BARRIER_SYNC_RESOLVE, D3D12_BARRIER_ACCESS_RESOLVE_DEST | D3D12_BARRIER_ACCESS_RESOLVE_SOURCE },
                { D3D12_BARRIER_SYNC_EXECUTE_INDIRECT, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT },
                { D3D12_BARRIER_SYNC_ALL_SHADING,
                  D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS |
                      D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_STREAM_OUTPUT },
                { D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS },
            } };

            //==========================================================================================================
            /// Returns the sync before of a buffer or texture barrier whose sides lowered to `before` and `after`, the
            /// first with access `access`: `before`, or, when the caller named no source stage, the destination's scope, or
            /// every scope when the destination has none too. GPU-based validation rejects `SYNC_NONE` before a resource
            /// any earlier command touched, in any earlier ExecuteCommandLists too, which RojoRHI does not track.
            D3D12_BARRIER_SYNC resourceSyncBefore(D3D12_BARRIER_SYNC before, D3D12_BARRIER_SYNC after, D3D12_BARRIER_ACCESS access)
            {
                if (before != D3D12_BARRIER_SYNC_NONE)
                {
                    return before;
                }
                return narrowSync(after != D3D12_BARRIER_SYNC_NONE ? after : D3D12_BARRIER_SYNC_ALL, access);
            }

            //==========================================================================================================
            /// Returns the access bits a list of `type` can perform: copy accesses on a Copy list, and on a Compute list
            /// every access but the render-target, depth-stencil, resolve and input-assembly ones.
            D3D12_BARRIER_ACCESS queueAccesses(QueueType type)
            {
                switch (type)
                {
                case QueueType::Compute:
                    return D3D12_BARRIER_ACCESS_CONSTANT_BUFFER | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE |
                           D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT | D3D12_BARRIER_ACCESS_COPY_DEST | D3D12_BARRIER_ACCESS_COPY_SOURCE;
                case QueueType::Copy: return D3D12_BARRIER_ACCESS_COPY_DEST | D3D12_BARRIER_ACCESS_COPY_SOURCE;
                default: return ~D3D12_BARRIER_ACCESS_NO_ACCESS;
                }
            }

            //==========================================================================================================
            /// Returns the access bits of one side on a list of `type`: none when its stage is `None`, which orders no earlier
            /// or later work. `Stage::All` performs every access, so a side may name one the queue never performs; it is
            /// dropped, and a side left with none names `NO_ACCESS`.
            D3D12_BARRIER_ACCESS sideAccess(Stage stage, Access access, AccessTarget target, Layout layout, QueueType type)
            {
                if (stage == Stage::None)
                {
                    return D3D12_BARRIER_ACCESS_NO_ACCESS;
                }
                const D3D12_BARRIER_ACCESS lowered = accessOf(access, target, layout);
                if (lowered == D3D12_BARRIER_ACCESS_NO_ACCESS)
                {
                    return lowered;
                }
                const D3D12_BARRIER_ACCESS performed = lowered & queueAccesses(type);
                return performed == D3D12_BARRIER_ACCESS_COMMON ? D3D12_BARRIER_ACCESS_NO_ACCESS : performed;
            }

            //==========================================================================================================
            /// Returns the access bits the scopes of `sync` can run, every access for `SYNC_ALL`.
            D3D12_BARRIER_ACCESS accessesOfSync(D3D12_BARRIER_SYNC sync)
            {
                if ((sync & D3D12_BARRIER_SYNC_ALL) != D3D12_BARRIER_SYNC_NONE)
                {
                    return ~D3D12_BARRIER_ACCESS_NO_ACCESS;
                }
                D3D12_BARRIER_ACCESS accesses = D3D12_BARRIER_ACCESS_COMMON;
                for (const SyncAccess& entry : kSyncAccesses)
                {
                    if ((sync & entry.sync) != D3D12_BARRIER_SYNC_NONE)
                    {
                        accesses |= entry.access;
                    }
                }
                return accesses;
            }

            //==========================================================================================================
            /// Returns the accesses a texture subresource in `layout` admits and the scopes they run in.
            SyncAccess layoutUse(Layout layout)
            {
                switch (layout)
                {
                case Layout::General:
                case Layout::Present:
                    return { D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_ALL_SHADING,
                             D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_COPY_SOURCE | D3D12_BARRIER_ACCESS_COPY_DEST };
                case Layout::ShaderRead:
                case Layout::DepthStencilRead: return { D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE };
                case Layout::ShaderWrite: return { D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
                case Layout::ColorAttachment: return { D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET };
                case Layout::DepthStencilWrite:
                    return { D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE };
                case Layout::CopySrc: return { D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE };
                case Layout::CopyDst: return { D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST };
                default: return { D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS };
                }
            }

            //==========================================================================================================
            /// Returns the sync and access of a texture side in `layout`, not `Undefined`, that names no access, `sync` its
            /// narrowed sync, on a list of `type`. The layer accepts no access in a defined layout only with `SYNC_NONE`, and
            /// GPU-based validation rejects `SYNC_NONE` before a subresource an earlier command touched, so the side names
            /// the accesses the layout admits instead: those its scopes run, or, when its scopes run none or not all of
            /// them, every scope; a side with no scope takes the layout's own. A layout the queue admits no access in keeps
            /// the side as it is.
            SyncAccess layoutSide(D3D12_BARRIER_SYNC sync, Layout layout, QueueType type)
            {
                const SyncAccess use = layoutUse(layout);
                const D3D12_BARRIER_ACCESS admitted = use.access & queueAccesses(type);
                if (admitted == D3D12_BARRIER_ACCESS_COMMON)
                {
                    return { sync, D3D12_BARRIER_ACCESS_NO_ACCESS };
                }
                if (sync == D3D12_BARRIER_SYNC_NONE)
                {
                    return { use.sync, admitted };
                }
                const D3D12_BARRIER_ACCESS run = admitted & accessesOfSync(sync);
                if (run != D3D12_BARRIER_ACCESS_COMMON && narrowSync(sync, run) == sync)
                {
                    return { sync, run };
                }
                return { D3D12_BARRIER_SYNC_ALL, admitted };
            }
        } // namespace

        //==============================================================================================================
        D3D12_BARRIER_SYNC copyStageSync(QueueType type)
        {
            switch (type)
            {
            case QueueType::Compute: return D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;
            case QueueType::Copy: return D3D12_BARRIER_SYNC_COPY;
            default:
                // A copy, the fill kernel, and a clear through a render-target, depth-stencil or unordered-access view.
                return D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_RENDER_TARGET | D3D12_BARRIER_SYNC_DEPTH_STENCIL |
                       D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;
            }
        }

        //==============================================================================================================
        D3D12_BARRIER_SYNC syncOf(Stage stages, QueueType type)
        {
            if (stages == Stage::None)
            {
                return D3D12_BARRIER_SYNC_NONE;
            }
            if (stages == Stage::All)
            {
                return D3D12_BARRIER_SYNC_ALL;
            }
            D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
            if ((stages & Stage::AllGraphics) == Stage::AllGraphics)
            {
                sync |= D3D12_BARRIER_SYNC_DRAW | D3D12_BARRIER_SYNC_EXECUTE_INDIRECT | D3D12_BARRIER_SYNC_RESOLVE;
                stages = stages & ~Stage::AllGraphics;
            }
            if (any(stages, Stage::IndirectArgs))
            {
                sync |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
            }
            if (any(stages, Stage::IndexInput))
            {
                sync |= D3D12_BARRIER_SYNC_INDEX_INPUT;
            }
            if (any(stages, Stage::VertexShader | Stage::TaskShader | Stage::MeshShader))
            {
                sync |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
            }
            if (any(stages, Stage::FragmentShader))
            {
                sync |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
            }
            if (any(stages, Stage::ComputeShader))
            {
                sync |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            }
            if (any(stages, Stage::ColorAttachment))
            {
                sync |= D3D12_BARRIER_SYNC_RENDER_TARGET;
            }
            if (any(stages, Stage::DepthStencil))
            {
                sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
            }
            if (any(stages, Stage::Copy))
            {
                sync |= copyStageSync(type);
            }
            if (any(stages, Stage::Resolve))
            {
                // A resolve target is in LAYOUT_RENDER_TARGET outside the pass's own resolve transitions, whose render-target
                // scope a barrier naming the resolve must chain with.
                sync |= D3D12_BARRIER_SYNC_RENDER_TARGET | D3D12_BARRIER_SYNC_RESOLVE;
            }
            return sync;
        }

        //==============================================================================================================
        D3D12_BARRIER_ACCESS accessOf(Access access, AccessTarget target, Layout layout)
        {
            // A subresource in Undefined holds nothing to make visible.
            if (access == Access::None || (target == AccessTarget::Texture && layout == Layout::Undefined))
            {
                return D3D12_BARRIER_ACCESS_NO_ACCESS;
            }
            D3D12_BARRIER_ACCESS lowered = D3D12_BARRIER_ACCESS_COMMON;
            if (any(access, Access::IndirectArgs))
            {
                lowered |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
            }
            if (any(access, Access::IndexRead))
            {
                lowered |= D3D12_BARRIER_ACCESS_INDEX_BUFFER;
            }
            if (any(access, Access::ShaderRead))
            {
                if (target != AccessTarget::Texture)
                {
                    lowered |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
                }
                else if (layout == Layout::ShaderWrite)
                {
                    // A texture in ShaderWrite is read through its Storage view.
                    lowered |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
                }
                else
                {
                    lowered |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
                }
            }
            if (any(access, Access::ShaderWrite))
            {
                lowered |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            }
            if (any(access, Access::ColorAttachmentRead | Access::ColorAttachmentWrite))
            {
                lowered |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
            }
            if (any(access, Access::DepthStencilRead))
            {
                lowered |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
            }
            if (any(access, Access::DepthStencilWrite))
            {
                lowered |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
            }
            if (any(access, Access::CopyRead))
            {
                lowered |= D3D12_BARRIER_ACCESS_COPY_SOURCE;
            }
            if (any(access, Access::CopyWrite))
            {
                lowered |= D3D12_BARRIER_ACCESS_COPY_DEST;
                if (target == AccessTarget::Global || target == AccessTarget::WritableBuffer)
                {
                    // fillBuffer writes through the buffer's raw unordered-access view from the backend's kernel.
                    lowered |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
                }
            }
            if (any(access, Access::Resolve))
            {
                // A resolve target is a ColorAttachment texture: only the pass's own transitions name RESOLVE_DEST.
                lowered |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
            }
            return lowered;
        }

        //==============================================================================================================
        D3D12_BARRIER_SYNC narrowSync(D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access)
        {
            if (access == D3D12_BARRIER_ACCESS_COMMON || sync == D3D12_BARRIER_SYNC_NONE || (sync & D3D12_BARRIER_SYNC_ALL) != D3D12_BARRIER_SYNC_NONE)
            {
                return sync;
            }
            if (access == D3D12_BARRIER_ACCESS_NO_ACCESS)
            {
                // A side with no access keeps every scope but the unordered-access clear's, which exists only for its
                // access: the layer rejects it without one.
                const D3D12_BARRIER_SYNC kept = sync & ~D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;
                return kept == D3D12_BARRIER_SYNC_NONE ? D3D12_BARRIER_SYNC_ALL : kept;
            }
            D3D12_BARRIER_SYNC kept = D3D12_BARRIER_SYNC_NONE;
            D3D12_BARRIER_SYNC known = D3D12_BARRIER_SYNC_NONE;
            for (const SyncAccess& entry : kSyncAccesses)
            {
                known |= entry.sync;
                if ((sync & entry.sync) != D3D12_BARRIER_SYNC_NONE && (access & entry.access) != D3D12_BARRIER_ACCESS_COMMON)
                {
                    kept |= entry.sync;
                }
            }
            // A bit the table does not name is kept as the caller gave it.
            kept |= sync & ~known;
            return kept == D3D12_BARRIER_SYNC_NONE ? D3D12_BARRIER_SYNC_ALL : kept;
        }

        //==============================================================================================================
        D3D12_BARRIER_LAYOUT layoutOf(Layout layout)
        {
            switch (layout)
            {
            case Layout::General: return D3D12_BARRIER_LAYOUT_COMMON;
            // The generic form, which initial data leaves a texture in and both direct and compute queues accept.
            case Layout::ShaderRead: return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
            case Layout::ShaderWrite: return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
            case Layout::ColorAttachment: return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
            case Layout::DepthStencilWrite: return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
            // The layout permits shader reads alone, which LAYOUT_DEPTH_STENCIL_READ does not admit.
            case Layout::DepthStencilRead: return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
            case Layout::CopySrc: return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
            case Layout::CopyDst: return D3D12_BARRIER_LAYOUT_COPY_DEST;
            case Layout::Present: return D3D12_BARRIER_LAYOUT_PRESENT;
            default: return D3D12_BARRIER_LAYOUT_UNDEFINED;
            }
        }

        //==============================================================================================================
        D3D12_BARRIER_SUBRESOURCE_RANGE rangeOf(const TexturePayload& payload, const SubresourceRange& range)
        {
            D3D12_BARRIER_SUBRESOURCE_RANGE lowered = {};
            lowered.IndexOrFirstMipLevel = range.baseMip;
            lowered.NumMipLevels = range.mipCount == kAllMips ? payload.mips - range.baseMip : range.mipCount;
            if (payload.type == TextureType::Tex3D)
            {
                // The depth slices of a 3D texture are one subresource.
                lowered.FirstArraySlice = 0;
                lowered.NumArraySlices = 1;
            }
            else
            {
                lowered.FirstArraySlice = range.baseLayer;
                lowered.NumArraySlices = range.layerCount == kAllLayers ? payload.layers - range.baseLayer : range.layerCount;
            }
            // Depth and stencil share one layout, so a barrier moves both planes.
            lowered.FirstPlane = 0;
            lowered.NumPlanes = core::hasStencilPlane(payload.format) ? 2u : 1u;
            return lowered;
        }
    } // namespace d3d12

    //==================================================================================================================
    void CommandList::barrier(std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers, std::span<const TextureBarrier> textures)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::barrier(list, globals, buffers, textures);
        if (globals.empty() && buffers.empty() && textures.empty())
        {
            return;
        }
        d3d12::ListState& state = impl.state;
        d3d12::BarrierScratch& scratch = state.barriers;
        scratch.globals.clear();
        scratch.buffers.clear();
        scratch.textures.clear();
        const QueueType type = list.type;
        for (const GlobalBarrier& global : globals)
        {
            D3D12_GLOBAL_BARRIER& lowered = scratch.globals.emplace_back();
            lowered.AccessBefore = d3d12::sideAccess(global.srcStage, global.srcAccess, d3d12::AccessTarget::Global, Layout::Undefined, type);
            lowered.AccessAfter = d3d12::sideAccess(global.dstStage, global.dstAccess, d3d12::AccessTarget::Global, Layout::Undefined, type);
            lowered.SyncBefore = d3d12::narrowSync(d3d12::syncOf(global.srcStage, type), lowered.AccessBefore);
            lowered.SyncAfter = d3d12::narrowSync(d3d12::syncOf(global.dstStage, type), lowered.AccessAfter);
        }
        // Payloads are fixed while their resources live, so each is read with one acquire load and no lock (ADR 0020).
        const auto& bufferPool = list.device->pool<Buffer>();
        for (const BufferBarrier& buffer : buffers)
        {
            const auto* payload = static_cast<const d3d12::BufferPayload*>(bufferPool.native(buffer.buffer));
            const d3d12::AccessTarget target = payload->viewCount == 2 ? d3d12::AccessTarget::WritableBuffer : d3d12::AccessTarget::Buffer;
            D3D12_BUFFER_BARRIER& lowered = scratch.buffers.emplace_back();
            lowered.AccessBefore = d3d12::sideAccess(buffer.srcStage, buffer.srcAccess, target, Layout::Undefined, type);
            lowered.AccessAfter = d3d12::sideAccess(buffer.dstStage, buffer.dstAccess, target, Layout::Undefined, type);
            lowered.SyncAfter = d3d12::narrowSync(d3d12::syncOf(buffer.dstStage, type), lowered.AccessAfter);
            lowered.SyncBefore = d3d12::resourceSyncBefore(
                d3d12::narrowSync(d3d12::syncOf(buffer.srcStage, type), lowered.AccessBefore), lowered.SyncAfter, lowered.AccessBefore
            );
            lowered.pResource = payload->resource;
            // Enhanced buffer barriers take the whole buffer: the range is widened, ordering more than asked, never less.
            lowered.Offset = 0;
            lowered.Size = UINT64_MAX;
        }
        const auto& texturePool = list.device->pool<Texture>();
        for (const TextureBarrier& texture : textures)
        {
            if (type == QueueType::Copy)
            {
                // A copy queue takes no layout barrier, and a Copy list's textures stay in General: the barrier orders
                // their memory as a global barrier does.
                D3D12_GLOBAL_BARRIER& global = scratch.globals.emplace_back();
                global.AccessBefore = d3d12::sideAccess(texture.srcStage, texture.srcAccess, d3d12::AccessTarget::Texture, texture.oldLayout, type);
                global.AccessAfter = d3d12::sideAccess(texture.dstStage, texture.dstAccess, d3d12::AccessTarget::Texture, texture.newLayout, type);
                global.SyncBefore = d3d12::narrowSync(d3d12::syncOf(texture.srcStage, type), global.AccessBefore);
                global.SyncAfter = d3d12::narrowSync(d3d12::syncOf(texture.dstStage, type), global.AccessAfter);
                continue;
            }
            const auto* payload = static_cast<const d3d12::TexturePayload*>(texturePool.native(texture.texture));
            D3D12_TEXTURE_BARRIER& lowered = scratch.textures.emplace_back();
            lowered.AccessAfter = d3d12::sideAccess(texture.dstStage, texture.dstAccess, d3d12::AccessTarget::Texture, texture.newLayout, type);
            lowered.SyncAfter = d3d12::narrowSync(d3d12::syncOf(texture.dstStage, type), lowered.AccessAfter);
            if (lowered.AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS && lowered.SyncAfter != D3D12_BARRIER_SYNC_NONE)
            {
                // An execution-only destination in a defined layout names the accesses the layout admits.
                const d3d12::SyncAccess after = d3d12::layoutSide(lowered.SyncAfter, texture.newLayout, type);
                lowered.SyncAfter = after.sync;
                lowered.AccessAfter = after.access;
            }
            lowered.AccessBefore = d3d12::sideAccess(texture.srcStage, texture.srcAccess, d3d12::AccessTarget::Texture, texture.oldLayout, type);
            const D3D12_BARRIER_SYNC before = d3d12::narrowSync(d3d12::syncOf(texture.srcStage, type), lowered.AccessBefore);
            if (lowered.AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS && texture.oldLayout != Layout::Undefined)
            {
                // A source with no access out of a defined layout names the accesses the layout admits.
                const d3d12::SyncAccess source = d3d12::layoutSide(before, texture.oldLayout, type);
                lowered.SyncBefore = source.sync;
                lowered.AccessBefore = source.access;
            }
            else
            {
                lowered.SyncBefore = d3d12::resourceSyncBefore(before, lowered.SyncAfter, lowered.AccessBefore);
            }
            lowered.LayoutBefore = d3d12::layoutOf(texture.oldLayout);
            lowered.LayoutAfter = d3d12::layoutOf(texture.newLayout);
            lowered.pResource = payload->resource;
            lowered.Subresources = d3d12::rangeOf(*payload, texture.range);
            // A barrier from Undefined names LAYOUT_UNDEFINED and discards whatever layout the texture is in, which also
            // initializes the compression metadata of a render-target, depth-stencil or unordered-access texture.
            lowered.Flags = texture.oldLayout == Layout::Undefined ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD : D3D12_TEXTURE_BARRIER_FLAG_NONE;
        }
        // One group per non-empty kind: the debug layer warns on an empty group.
        std::array<D3D12_BARRIER_GROUP, 3> groups = {};
        UINT32 count = 0;
        if (!scratch.globals.empty())
        {
            groups[count].Type = D3D12_BARRIER_TYPE_GLOBAL;
            groups[count].NumBarriers = static_cast<UINT32>(scratch.globals.size());
            groups[count].pGlobalBarriers = scratch.globals.data();
            ++count;
        }
        if (!scratch.buffers.empty())
        {
            groups[count].Type = D3D12_BARRIER_TYPE_BUFFER;
            groups[count].NumBarriers = static_cast<UINT32>(scratch.buffers.size());
            groups[count].pBufferBarriers = scratch.buffers.data();
            ++count;
        }
        if (!scratch.textures.empty())
        {
            groups[count].Type = D3D12_BARRIER_TYPE_TEXTURE;
            groups[count].NumBarriers = static_cast<UINT32>(scratch.textures.size());
            groups[count].pTextureBarriers = scratch.textures.data();
            ++count;
        }
        state.list->Barrier(count, groups.data());
        core::noteNativeCall(list);
        ++list.counts.nativeBarriers;
    }
} // namespace rojoRHI
