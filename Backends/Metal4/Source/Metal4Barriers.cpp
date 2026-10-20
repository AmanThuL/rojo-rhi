//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Barriers.cpp
/// @brief Implements the lowering of barrier batches to Metal 4 stage barriers: pending, encoder-level and trailing.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Barriers.h"

#include <rojoRHI/RHI.h>

#include "Core/Barriers.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Encoders.h"
#include "Metal4ListState.h"

#include <Metal/Metal.hpp>

#include <span>

namespace rojoRHI {
    namespace metal4 {
        namespace {
            //==========================================================================================================
            /// Returns true when `stages` and `mask` share a bit.
            bool any(Stage stages, Stage mask)
            {
                return (stages & mask) != Stage::None;
            }

            /// The Metal stages, the source and destination stages and the visibility of one barrier batch.
            struct Lowered
            {
                MTL::Stages source = 0;                 ///< The union of the source stages.
                MTL::Stages destination = 0;            ///< The union of the destination stages.
                MTL4::VisibilityOptions visibility = 0; ///< `Device` when any access is named, `ResourceAlias` for an alias.
            };

            //==========================================================================================================
            /// Adds one barrier's sides to `lowered`.
            void addSides(Lowered& lowered, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
            {
                lowered.source |= metalStages(srcStage);
                lowered.destination |= metalStages(dstStage);
                if (srcAccess != Access::None || dstAccess != Access::None)
                {
                    lowered.visibility |= MTL4::VisibilityOptionDevice;
                }
            }
        } // namespace

        //==============================================================================================================
        MTL::Stages metalStages(Stage stages)
        {
            if (stages == Stage::All)
            {
                return MTL::StageAll;
            }
            MTL::Stages lowered = 0;
            if (any(stages, Stage::IndirectArgs))
            {
                // Metal has no argument stage: the stages that consume arguments stand for it.
                lowered |= MTL::StageDispatch | MTL::StageVertex | MTL::StageObject | MTL::StageMesh;
            }
            if (any(stages, Stage::IndexInput | Stage::VertexShader))
            {
                lowered |= MTL::StageVertex;
            }
            if (any(stages, Stage::TaskShader))
            {
                lowered |= MTL::StageObject;
            }
            if (any(stages, Stage::MeshShader))
            {
                lowered |= MTL::StageMesh;
            }
            if (any(stages, Stage::FragmentShader | Stage::ColorAttachment | Stage::DepthStencil | Stage::Resolve))
            {
                lowered |= MTL::StageFragment;
            }
            if (any(stages, Stage::ComputeShader))
            {
                lowered |= MTL::StageDispatch;
            }
            if (any(stages, Stage::Copy))
            {
                // A copy, fill or clear runs as a blit, a backend kernel or a render-pass load action.
                lowered |= MTL::StageBlit | MTL::StageDispatch | MTL::StageFragment;
            }
            return lowered;
        }

        //==============================================================================================================
        void emitAtOpen(core::CommandListCore& list, BarrierState& barriers, MTL4::CommandEncoder* encoder, MTL::Stages stages)
        {
            // A render encoder emits for its fragment stage alone, a compute encoder for its own stages; the wait names every
            // stage of the encoder's kind.
            const MTL::Stages kind = (stages & kRenderStages) != 0 ? kRenderStages : kComputeStages;
            if ((barriers.owedFenceStages & kind) != 0)
            {
                // MetalFX's own encoders are ordered only through the fence: the barriers before an upscale reach the work of
                // either kind after it, and a barrier after it reaches MetalFX's writes, through this wait. Each kind waits once.
                encoder->waitForFence(barriers.owedFence, kind);
                core::noteNativeCall(list);
                barriers.owedFenceStages &= ~kind;
            }
            const MTL::Stages owed = barriers.pendingDestination & stages;
            if (owed == 0)
            {
                barriers.clearOpen();
                return;
            }
            encoder->barrierAfterQueueStages(barriers.pendingSource, owed, barriers.pendingVisibility);
            core::noteNativeCall(list);
            ++list.counts.nativeBarriers;
            barriers.openSource = barriers.pendingSource;
            barriers.openDestination = owed;
            barriers.openVisibility = barriers.pendingVisibility;
            barriers.pendingDestination &= ~stages;
            if (barriers.pendingDestination == 0)
            {
                barriers.pendingSource = 0;
                barriers.pendingVisibility = 0;
            }
        }

        //==============================================================================================================
        void emitDeferred(core::CommandListCore& list, BarrierState& barriers, MTL4::CommandEncoder* encoder, MTL::Stages stages, MTL::Stages command)
        {
            const MTL::Stages held = barriers.deferredDestination & command;
            if (held == 0)
            {
                return;
            }
            const MTL::Stages after = barriers.deferredSource & stages;
            if (after != 0)
            {
                encoder->barrierAfterEncoderStages(after, held, barriers.deferredVisibility);
                core::noteNativeCall(list);
                ++list.counts.nativeBarriers;
            }
            // The stages no command has run in since stay held, for a later command in them or, if the encoder closes first,
            // for the pending barrier.
            barriers.deferredDestination &= ~held;
            if (barriers.deferredDestination == 0)
            {
                barriers.deferredSource = 0;
                barriers.deferredVisibility = 0;
            }
        }

        //==============================================================================================================
        void emitTrailing(core::CommandListCore& list, ListState& state, BarrierState& barriers, MTL::Fence* carry)
        {
            // A list that upscaled ends with a wait on the last upscale's fence even when no barrier is pending: later lists see
            // MetalFX's writes only through the encoder that waits on it.
            const bool fenced = barriers.owedFence != nullptr;
            if (barriers.pendingDestination == 0 && !fenced)
            {
                return;
            }
            {
                // The factory autoreleases its encoder, which is used and ended inside the pool. It binds no table: it runs no
                // command, only the wait and the two halves of the barrier.
                const AutoreleasePool pool;
                MTL4::ComputeCommandEncoder* encoder = state.commandBuffer->computeCommandEncoder();
                core::noteNativeCall(list);
                ++list.counts.encodersOpened;
                encoder->setLabel(state.label);
                core::noteNativeCall(list);
                MTL::Stages destination = barriers.pendingDestination;
                MTL4::VisibilityOptions visibility = barriers.pendingVisibility;
                if (fenced)
                {
                    // The work held back follows MetalFX's encoders of the list's last upscale, which only the fence orders;
                    // MetalFX's stages are its own, so every later stage waits.
                    encoder->waitForFence(barriers.owedFence, kComputeStages);
                    core::noteNativeCall(list);
                    destination = MTL::StageAll;
                    visibility |= MTL4::VisibilityOptionDevice;
                }
                if (barriers.pendingDestination != 0)
                {
                    // The consumer half: the encoder's stages wait for every earlier source on the queue.
                    encoder->barrierAfterQueueStages(barriers.pendingSource, kComputeStages, barriers.pendingVisibility);
                    core::noteNativeCall(list);
                    ++list.counts.nativeBarriers;
                }
                const MTL::Stages carried = fenced ? MTL::Stages{ 0 } : fencedStages(barriers, carry);
                if (carried != 0)
                {
                    // Later dispatches wait on the fence, which a counted draw's kernels do not: a barrier to the dispatch
                    // stage would hold them back too.
                    encoder->updateFence(carry, kComputeStages);
                    core::noteNativeCall(list);
                    destination &= ~carried;
                }
                if (destination != 0)
                {
                    // The producer half: every later encoder on the queue waits in these stages for the encoder.
                    encoder->barrierAfterStages(kComputeStages, destination, visibility);
                    core::noteNativeCall(list);
                    ++list.counts.nativeBarriers;
                }
                encoder->endEncoding();
                core::noteNativeCall(list);
            }
            barriers.reset();
        }
    } // namespace metal4

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
        metal4::Lowered lowered;
        for (const GlobalBarrier& global : globals)
        {
            metal4::addSides(lowered, global.srcStage, global.srcAccess, global.dstStage, global.dstAccess);
        }
        const core::AliasTracker& aliasing = list.device->aliasing();
        if (!globals.empty() && aliasing.anyAliasing())
        {
            // A global barrier is how the first use of a placed buffer is ordered after the resource that held its memory, which
            // only matters while placed resources share memory or sparse tiles can name it.
            lowered.visibility |= MTL4::VisibilityOptionResourceAlias;
        }
        for (const BufferBarrier& buffer : buffers)
        {
            metal4::addSides(lowered, buffer.srcStage, buffer.srcAccess, buffer.dstStage, buffer.dstAccess);
        }
        for (const TextureBarrier& texture : textures)
        {
            metal4::addSides(lowered, texture.srcStage, texture.srcAccess, texture.dstStage, texture.dstAccess);
            if (texture.oldLayout == Layout::Undefined && (lowered.visibility & MTL4::VisibilityOptionResourceAlias) == 0)
            {
                // A discard of a placed texture may follow another resource in its memory. A texture whose range never overlapped
                // another's needs no alias unless sparse tiles are live, which can name any placed memory. The payload is read
                // without a lock.
                bool aliases = aliasing.textureAliases(texture.texture);
                if (!aliases && aliasing.anySparse())
                {
                    const auto* native = static_cast<const MTL::Texture*>(list.device->pool<Texture>().native(texture.texture));
                    aliases = native != nullptr && native->hazardTrackingMode() == MTL::HazardTrackingModeUntracked;
                }
                if (aliases)
                {
                    lowered.visibility |= MTL4::VisibilityOptionResourceAlias;
                }
            }
        }
        // A stage no encoder of the queue runs names no work: a graphics queue runs compute and render encoders, a compute
        // queue compute encoders, and a copy queue only the blits of its copies.
        MTL::Stages queueStages = metal4::kComputeStages | metal4::kRenderStages;
        if (list.type == QueueType::Compute)
        {
            queueStages = metal4::kComputeStages;
        }
        else if (list.type == QueueType::Copy)
        {
            queueStages = MTL::StageBlit;
        }
        lowered.source &= queueStages;
        lowered.destination &= queueStages;
        if (lowered.source == 0 || lowered.destination == 0)
        {
            // No earlier work to wait for, or no later work to hold back: an execution-only barrier with nothing to order.
            return;
        }
        metal4::Encoders& encoders = impl.encoders;
        metal4::BarrierState& barriers = encoders.barriers();
        if (!barriers.argsNamed)
        {
            for (const GlobalBarrier& global : globals)
            {
                barriers.argsNamed = barriers.argsNamed || metal4::any(global.dstStage, Stage::IndirectArgs);
            }
            for (const BufferBarrier& buffer : buffers)
            {
                barriers.argsNamed = barriers.argsNamed || metal4::any(buffer.dstStage, Stage::IndirectArgs);
            }
            for (const TextureBarrier& texture : textures)
            {
                barriers.argsNamed = barriers.argsNamed || metal4::any(texture.dstStage, Stage::IndirectArgs);
            }
        }
        if (encoders.lingering())
        {
            // The ended pass's encoder runs no further command, so the batch orders only later encoders: it joins the pending
            // barrier, which the next encoder emits at its opening or the lingering encoder carries at `end`.
            barriers.merge(lowered.source, lowered.destination, lowered.visibility);
            return;
        }
        const MTL::Stages open = encoders.stages();
        const MTL::Stages covered = lowered.destination & open;
        if (encoders.holdsWork() && (lowered.source & ~barriers.openSource) == 0 && (covered & ~barriers.openDestination) == 0)
        {
            // Every source since the open encoder's opening barrier lies in the encoder, and that barrier already holds the
            // encoder's destination stages for earlier work: the barrier stays in the encoder, emitted before its next
            // command in a destination stage, or queue-level if the encoder closes first.
            if (covered != 0)
            {
                barriers.defer(lowered.source, covered, lowered.visibility);
            }
            if ((lowered.destination & ~open) != 0)
            {
                barriers.merge(lowered.source, lowered.destination & ~open, lowered.visibility);
            }
            return;
        }
        // A source may lie in an earlier encoder or an earlier list, which only a queue-level barrier orders. An open compute
        // encoder that has not yet run a command in the destination stages takes it, with the encoder-level part for its own
        // work; otherwise the open encoder ends, and the next encoders to open emit the barrier for their stages.
        if (encoders.holdsWork() && covered != 0 && encoders.barrierInOpen(list, lowered.source, covered, lowered.visibility))
        {
            if ((lowered.destination & ~open) != 0)
            {
                barriers.merge(lowered.source, lowered.destination & ~open, lowered.visibility);
            }
            return;
        }
        if (encoders.holdsWork())
        {
            encoders.close(list);
        }
        barriers.merge(lowered.source, lowered.destination, lowered.visibility);
    }
} // namespace rojoRHI
