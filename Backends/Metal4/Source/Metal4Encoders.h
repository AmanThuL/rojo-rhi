//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Encoders.h
/// @brief Declares the encoder a Metal 4 command list keeps open: the lazy compute encoder, a clear's render encoders and a render pass's.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/CommandLists.h"
#include "Metal4Barriers.h"
#include "Metal4ListState.h"

#include <Metal/Metal.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rojoRHI::metal4 {
    enum class EncoderKind : uint8_t
    {
        None,
        Compute, // Records copies, fills and backend kernels.
        Render,  // A clear's, which clears one subresource by its load actions, or a render pass's.
    };

    // Writer kernels of consecutive counted draws, run as one dispatch of a thread per draw; each draw's parameters lie
    // `stride` bytes after the last's.
    struct KernelRun
    {
        const MTL::ComputePipelineState* kernel = nullptr;
        const ListState* state = nullptr; // Its kernel table and pool setting.
        uint64_t params = 0;
        uint64_t stride = 0;
        uint32_t count = 0; // None when no run is open.
    };

    // The compute encoder opens on the first command that needs it and stays open until a scope boundary, a queue-level
    // barrier, a clear's render encoder, a render pass or `CommandList::end` closes it, so consecutive copies and fills
    // share it. A render pass's last encoder lingers after the pass ends, running no further command, so it may carry
    // the barrier the list still owes. Only the list's recording thread touches it.
    class Encoders
    {
      public:
        // A new encoder's factory runs inside an autorelease pool and the encoder is retained; it takes the list's
        // argument table unless `kernelsOnly`, then emits the pending barrier's part on its stages. A `kernelsOnly`
        // encoder runs only backend kernels: a render pass's counted draws open it ahead of the render encoder, and
        // nothing else records on a compute encoder inside a pass (chapter 07). Any other encoder of a list that uses
        // the carry fence waits on it once, before its first dispatch-stage command; a kernels-only encoder never
        // waits. A lingering render encoder ends first.
        MTL4::ComputeCommandEncoder* compute(core::CommandListCore& list, ListState& state, MTL::Stages command, bool kernelsOnly = false)
        {
            // A command on an open compute encoder that holds work, with no carry wait owed before it, needs at most the
            // encoder-level barrier held back for its stage first: no other test and no call but the barrier's.
            if (m_compute != nullptr && m_holdsWork && !owesCarryWait(command))
            {
                if ((m_barriers.deferredDestination & command) != 0)
                {
                    emitDeferred(list, m_barriers, m_compute, kComputeStages, command);
                }
                return m_compute;
            }
            return prepareCompute(list, state, command, kernelsOnly);
        }

        // Joins the open run when that run runs `kernel` and its next draw's parameters would lie at `params`;
        // otherwise the open run is dispatched and a new one opens. Counts nothing until the run is dispatched.
        void queueKernel(core::CommandListCore& list, const ListState& state, const MTL::ComputePipelineState* kernel, uint64_t params, uint64_t stride);

        void dispatchRun(core::CommandListCore& list);

        // Holds the barrier in the open encoder when it holds work, does not linger, every source lies among those its
        // opening barrier waited for, its destination stages among those that barrier held back and its visibility within
        // that barrier's; the stages it lacks join the pending barrier. Returns false, changing nothing, otherwise.
        bool holdInOpen(MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility)
        {
            if (m_lingering || !m_holdsWork)
            {
                return false;
            }
            const MTL::Stages open = stages();
            const MTL::Stages covered = destination & open;
            if ((source & ~m_barriers.openSource) != 0 || (covered & ~m_barriers.openDestination) != 0 || (visibility & ~m_barriers.openVisibility) != 0)
            {
                return false;
            }
            // Every source since the opening barrier lies in the encoder, and that barrier already holds the encoder's
            // destination stages for earlier work: the barrier stays in the encoder, emitted before its next command in a
            // destination stage, or queue-level if the encoder closes first.
            if (covered != 0)
            {
                m_barriers.defer(source, covered, visibility);
            }
            if ((destination & ~open) != 0)
            {
                m_barriers.merge(source, destination & ~open, visibility);
            }
            return true;
        }

        LastGlobal& lastGlobal()
        {
            return m_lastGlobal;
        }

        // The factory runs inside an autorelease pool and the encoder is retained. It binds no table, because no shader
        // runs on it, and emits only the pending barrier's part on the fragment stage, where its load and store actions
        // run.
        MTL4::RenderCommandEncoder* openRender(core::CommandListCore& list, ListState& state, const MTL4::RenderPassDescriptor* descriptor);

        // The factory runs inside the autorelease pool the caller holds. Emits the pending barrier's part on every render
        // stage; the table waits for `bindTable`. The encoder has run its fragment stage, where its load and store actions
        // run; `markRan` adds the rest.
        MTL4::RenderCommandEncoder* openPass(core::CommandListCore& list, ListState& state, const MTL4::RenderPassDescriptor* descriptor);

        // Every shader of a draw reads the reserved bindings through the list's table (ADR 0018), which the open render
        // encoder takes at the vertex and fragment stages before its first draw that is not an execute: an indirect
        // command buffer's commands bind their own buffers, so an encoder that only executes binds no table.
        void bindTable(core::CommandListCore& list, const ListState& state)
        {
            if (m_tableBound) [[likely]]
            {
                return;
            }
            m_render->setArgumentTable(state.table, MTL::RenderStageVertex | MTL::RenderStageFragment);
            core::noteNativeCall(list);
            m_tableBound = true;
        }

        // Its opening barrier's hold on `stages` is spent.
        void markRan(MTL::Stages stages)
        {
            m_ran |= stages;
        }

        // A compute encoder that waits on the fence an earlier upscale left owed and then for every stage of all
        // earlier work on the queue, then updates `fence` and ends. MetalFX's encoders wait on `fence`, so the barriers
        // before the upscale reach them through it. It binds no table, because no shader runs on it.
        void carry(core::CommandListCore& list, ListState& state, MTL::Fence* fence);

        // Emitted inside the open compute encoder instead of ending it, when its opening barrier covered nothing, it
        // runs more than backend kernels and no batch is pending, whose sources the pair would not name. Every later
        // command in `destination` follows the pair, so it counts as the encoder's opening barrier for the stages a
        // command follows it in; the stages the encoder ran before it stay apart (`BarrierState::preRan`). Returns
        // false, emitting nothing, otherwise.
        bool barrierInOpen(core::CommandListCore& list, MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility);

        // What is still held back joins the pending barrier now, as `close` would add it, while what its opening
        // covered and the stages it ran stay known for `finish`. `renderEncoder` no longer returns it.
        void linger();

        // Carries the barrier the list still owes on the open encoder when the encoder covers its sources: every
        // pending source lies among those its opening barrier waited for, and it ran a command in a stage that barrier
        // held after it. The producer barrier the encoder owes for the batches it kept folds into that carry: its sources
        // join the carry's, and its destinations join the carry's barrier, or the fence update where the fence may carry
        // them. A list that upscaled carries nothing here; `emitTrailing` emits what is owed.
        void finish(core::CommandListCore& list);

        // At the list's acquisition: the graphics queue's fence for a graphics list acquired once a counted draw's
        // kernels have run in an earlier frame, else null, so the list carries the dispatch stage in a barrier and
        // waits on no fence.
        void setCarry(MTL::Fence* fence, std::atomic<bool>* kernelsRan)
        {
            m_carryFence = fence;
            m_kernelsRan = kernelsRan;
        }

        // At the list's acquisition. A copy list runs only `MTL::StageBlit`: its commands are all blits.
        void setQueueStages(MTL::Stages stages)
        {
            m_queueStages = stages;
        }

        MTL::Stages queueStages() const
        {
            return m_queueStages;
        }

        MTL::Fence* carryFence() const
        {
            return m_carryFence;
        }

        bool lingering() const
        {
            return m_lingering;
        }

        bool carryWaited() const
        {
            return m_carryWaited;
        }

        void markWork()
        {
            m_holdsWork = true;
        }

        bool holdsWork() const
        {
            return m_holdsWork;
        }

        EncoderKind kind() const
        {
            return m_kind;
        }

        MTL4::ComputeCommandEncoder* computeEncoder() const
        {
            return m_compute;
        }

        MTL4::RenderCommandEncoder* renderEncoder() const
        {
            return m_lingering ? nullptr : m_render;
        }

        MTL::Stages stages() const
        {
            switch (m_kind)
            {
            case EncoderKind::Compute: return kComputeStages;
            case EncoderKind::Render: return kRenderStages;
            case EncoderKind::None: break;
            }
            return 0;
        }

        const MTL::ComputePipelineState* kernelPipeline() const
        {
            return m_kernelPipeline;
        }

        void setKernelPipeline(const MTL::ComputePipelineState* pipeline)
        {
            m_kernelPipeline = pipeline;
        }

        BarrierState& barriers()
        {
            return m_barriers;
        }

        const BarrierState& barriers() const
        {
            return m_barriers;
        }

        // An encoder-level barrier still held back joins the pending barrier, as do the stages its opening barrier held
        // back in which it ran no command. A render encoder ends inside an autorelease pool, because the validation
        // layer autoreleases there.
        void close(core::CommandListCore& list);

        // A list that never ended, which only a Release build lets through, leaves them behind. Opens an autorelease
        // pool only when there is something to release.
        void release();

        // The list keeps them retained until its frame slot is reused, as an autorelease pool drained once per frame
        // would, so no encoder is freed inside a recording. The caller holds an autorelease pool.
        void releaseEnded();

        size_t endedCount() const
        {
            return m_ended.size();
        }

      private:
        MTL4::ComputeCommandEncoder* prepareCompute(core::CommandListCore& list, ListState& state, MTL::Stages command, bool kernelsOnly);

        // The conditions `waitCarry` skips the wait on.
        bool owesCarryWait(MTL::Stages command) const
        {
            return !m_carryWaited && !m_kernelsOnly && m_carryFence != nullptr && (command & MTL::StageDispatch) != 0;
        }

        void settle();

        // On the compute encoder about to end: the producer barrier for the work its kept barriers spent.
        void endKept(core::CommandListCore& list);

        MTL::Stages ranStages() const;

        // The stages the encoder ran a command in after its opening barrier: all it ran, less those it ran only before a
        // barrier `barrierInOpen` made the opening one.
        MTL::Stages ranSinceOpen() const;

        // Unless the encoder waited already, runs only backend kernels, the command is not in the dispatch stage or the
        // list uses no carry fence.
        void waitCarry(core::CommandListCore& list, MTL::Stages command);

        MTL4::ComputeCommandEncoder* m_compute = nullptr; // Retained.
        MTL4::RenderCommandEncoder* m_render = nullptr;   // Retained.
        EncoderKind m_kind = EncoderKind::None;
        bool m_holdsWork = false;
        bool m_commandPools = false; // Taken when an encoder opens.
        MTL::Stages m_ran = 0;       // A render encoder has no stages getter, so it keeps them here.
        bool m_lingering = false;    // Its pass ended; it runs no further command.
        bool m_tableBound = false;   // The open render encoder holds the list's table at its vertex and fragment stages.
        bool m_kernelsOnly = false;
        bool m_carryWaited = false;
        bool m_syncsAtEnd = false;                 // The open encoder carried a barrier to later lists at `end`.
        MTL::Fence* m_carryFence = nullptr;        // Null but on a fenced graphics list.
        std::atomic<bool>* m_kernelsRan = nullptr; // Null on other lists.
        MTL::Stages m_queueStages = 0;
        BarrierState m_barriers;
        LastGlobal m_lastGlobal; // Kept across recordings.
        // Neither pipeline is set again while the encoder holds it, which the validation layer reports as redundant.
        // Every entry that sets a compute pipeline records it with `setKernelPipeline`.
        const MTL::ComputePipelineState* m_kernelPipeline = nullptr;
        KernelRun m_run;
        // Each retained until `releaseEnded`; the vector keeps its capacity.
        std::vector<MTL4::CommandEncoder*> m_ended;
    };
} // namespace rojoRHI::metal4
