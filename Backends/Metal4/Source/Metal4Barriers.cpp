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
            if ((barriers.deferredDestination & command) == 0)
            {
                return;
            }
            const MTL::Stages after = barriers.deferredSource & stages;
            if (after != 0)
            {
                encoder->barrierAfterEncoderStages(after, barriers.deferredDestination, barriers.deferredVisibility);
                core::noteNativeCall(list);
                ++list.counts.nativeBarriers;
            }
            barriers.deferredSource = 0;
            barriers.deferredDestination = 0;
            barriers.deferredVisibility = 0;
        }

        //==============================================================================================================
        void emitTrailing(core::CommandListCore& list, ListState& state, BarrierState& barriers)
        {
            if (barriers.pendingDestination == 0)
            {
                return;
            }
            {
                // The factory autoreleases its encoder, which is used and ended inside the pool. It binds no table: it runs no
                // command, only the two halves of the barrier.
                const AutoreleasePool pool;
                MTL4::ComputeCommandEncoder* encoder = state.commandBuffer->computeCommandEncoder();
                core::noteNativeCall(list);
                ++list.counts.encodersOpened;
                encoder->setLabel(state.label);
                core::noteNativeCall(list);
                // The consumer half: the encoder's stages wait for every earlier source on the queue.
                encoder->barrierAfterQueueStages(barriers.pendingSource, kComputeStages, barriers.pendingVisibility);
                core::noteNativeCall(list);
                // The producer half: every later encoder on the queue, in this list or a later one, waits for the encoder.
                encoder->barrierAfterStages(kComputeStages, barriers.pendingDestination, barriers.pendingVisibility);
                core::noteNativeCall(list);
                list.counts.nativeBarriers += 2;
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
        if (!globals.empty())
        {
            // A global barrier is how the first use of a placed buffer is ordered after the resource that held its memory.
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
                // Only placed textures are untracked; a discard of one may follow another resource in its memory. The payload
                // is read without a lock.
                const auto* native = static_cast<const MTL::Texture*>(list.device->pool<Texture>().native(texture.texture));
                if (native != nullptr && native->hazardTrackingMode() == MTL::HazardTrackingModeUntracked)
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
        // A source may lie in an earlier encoder or an earlier list, which only a queue-level barrier orders: the open encoder
        // ends, and the next encoders to open emit the barrier for their stages.
        if (encoders.holdsWork())
        {
            encoders.close(list);
        }
        barriers.merge(lowered.source, lowered.destination, lowered.visibility);
    }
} // namespace rojoRHI
