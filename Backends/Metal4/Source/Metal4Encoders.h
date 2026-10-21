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
    /// The kind of encoder a list has open.
    enum class EncoderKind : uint8_t
    {
        None,    ///< No encoder is open.
        Compute, ///< The compute encoder, which records copies, fills and backend kernels.
        Render,  ///< A render encoder: a clear's, which clears one subresource by its load actions, or a render pass's.
    };

    /// The one encoder a command list may have open. The compute encoder opens on the first command that needs it and stays
    /// open until a scope boundary, a queue-level barrier, a clear's render encoder, a render pass or `CommandList::end`
    /// closes it, so consecutive copies and fills share it. A render pass's last encoder stays open after the pass ends
    /// (it lingers), running no further command, until the next encoder opens, a scope boundary or `end`, which may carry
    /// the barrier the list still owes on it. Only the list's recording thread touches it.
    class Encoders
    {
      public:
        /// Returns the open compute encoder for a command about to be recorded in stage `command`, first emitting the part of
        /// the encoder-level barrier held back for that stage (and, while it holds no work, the part of the pending barrier on
        /// its stages, which a barrier recorded after it opened left there), or opens one when none is open: the factory runs
        /// inside an autorelease pool and the encoder is retained; it takes the innermost open scope's label, else the list's,
        /// and the list's argument table, unless `kernelsOnly`, then emits the pending barrier's part on its stages. The opening
        /// counts its native calls, its barrier and one encoder on `list`. The new encoder holds no work. An encoder opened
        /// `kernelsOnly` runs only backend kernels, which bind the kernel table (`bindKernelBatch`): a render pass's counted draws
        /// open it ahead of the render encoder, and nothing else records on a compute encoder inside a pass (chapter 07). Any
        /// other encoder of a list that uses the carry fence waits on it once, before its first command in the dispatch stage,
        /// so the dispatch part of a barrier an earlier list carried reaches it; a kernels-only encoder never waits. A
        /// lingering render encoder ends first.
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

        /// Holds back a barrier from `source` to `destination` with `visibility`, already intersected with the queue's stages,
        /// in the open encoder when it holds work, does not linger, every source lies among those its opening barrier waited
        /// for and the destination stages it has among those that barrier held back: the part on its stages as an
        /// encoder-level barrier held back until its next command in one of them, and the stages it lacks in the pending
        /// barrier. Returns false, changing nothing, otherwise.
        bool holdInOpen(MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility)
        {
            if (m_lingering || !m_holdsWork)
            {
                return false;
            }
            const MTL::Stages open = stages();
            const MTL::Stages covered = destination & open;
            if ((source & ~m_barriers.openSource) != 0 || (covered & ~m_barriers.openDestination) != 0)
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

        /// Returns the last batch of one global barrier the list lowered, which the next such batch reuses.
        LastGlobal& lastGlobal()
        {
            return m_lastGlobal;
        }

        /// Opens a render encoder from `descriptor` when no encoder is open: the factory runs inside an autorelease pool and the
        /// encoder is retained; it takes the innermost open scope's label, else the list's, and binds no table, because no
        /// shader runs on it; then it emits the pending barrier's part on the fragment stage, where its load and store actions
        /// run, and leaves the other render stages pending. The opening counts its native calls, its barrier and one encoder
        /// on `list`. The caller closes it with `close`. A lingering render encoder ends first.
        MTL4::RenderCommandEncoder* openRender(core::CommandListCore& list, ListState& state, const MTL4::RenderPassDescriptor* descriptor);

        /// Opens a render pass's encoder from `descriptor` when no encoder is open: the factory runs inside an autorelease pool
        /// and the encoder is retained; it takes the innermost open scope's label, else the list's, and binds the list's argument
        /// table to the vertex, fragment, object and mesh stages (ADR 0018), then emits the pending barrier's part on every
        /// render stage. The opening counts its native calls, its barrier and one encoder on `list`. The encoder has run its
        /// fragment stage, where its load and store actions run; `markRan` adds the stages its commands run in. A lingering
        /// render encoder ends first.
        MTL4::RenderCommandEncoder* openPass(core::CommandListCore& list, ListState& state, const MTL4::RenderPassDescriptor* descriptor);

        /// Notes that the open render encoder ran a command in `stages`, so its opening barrier's hold on them is spent.
        void markRan(MTL::Stages stages)
        {
            m_ran |= stages;
        }

        /// Records the carrier of an upscale when no encoder is open: a compute encoder that waits on the fence an earlier
        /// upscale left owed to either encoder kind and then for every stage of all earlier work on the queue, which covers the
        /// pending barrier's part on its stages, then updates `fence`
        /// after those stages and ends. MetalFX's encoders wait on `fence`, so the barriers before the upscale reach them
        /// through it. The encoder takes the innermost open scope's label, else the list's, binds no table, because no shader
        /// runs on it, and is used and ended inside the autorelease pool its factory needs. Counts its native calls, its
        /// barrier and one encoder on `list`. A lingering render encoder ends first.
        void carry(core::CommandListCore& list, ListState& state, MTL::Fence* fence);

        /// Emits a barrier from `source` to `destination`, which are stages of the open compute encoder, inside that encoder
        /// instead of ending it, when its opening barrier covered nothing, it runs more than backend kernels and it ran no
        /// command in `destination` yet: a queue-level barrier holds its later commands in `destination` for earlier encoders'
        /// work in `source`, and the encoder-level part for its own work is held back until a command in `destination`, as a
        /// covered barrier is. Since every command of the encoder in `destination` follows the pair, it then counts as the
        /// encoder's opening barrier. Counts the queue-level barrier on `list`. Returns false, emitting nothing, otherwise.
        bool barrierInOpen(core::CommandListCore& list, MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility);

        /// Keeps the open render pass encoder open after its pass ended, running no further command: an encoder-level barrier
        /// still held back and the stages its opening barrier held in which it ran no command join the pending barrier now, as
        /// `close` would add them, while what its opening covered and the stages it ran stay known for `finish`.
        /// `renderEncoder` no longer returns it.
        void linger();

        /// Ends the open encoder at `CommandList::end`, first carrying on it the barrier the list still owes when the encoder
        /// covers that barrier's sources: every pending source stage lies among those its opening barrier waited for at queue
        /// level, and it ran a command in a stage that barrier held. The encoder then updates the carry fence after the stages
        /// it ran, for the dispatch stage of the destination of a list that uses the fence, unless a barrier of the list named `IndirectArgs`,
        /// and emits `barrierAfterStages` from those stages to the destination stages left, which holds them back in every
        /// later encoder on the queue. Counts its calls and barrier on `list`. A list that upscaled carries nothing here;
        /// `emitTrailing` emits what is owed. Does nothing when no encoder is open.
        void finish(core::CommandListCore& list);

        /// Sets, at the list's acquisition, the fence later dispatches wait on and a closing encoder updates: the graphics
        /// queue's for a graphics list acquired once a counted draw's kernels have run in an earlier frame, else null, so the
        /// list carries the dispatch stage in a barrier and waits on no fence. `kernelsRan` is the graphics queue's flag a
        /// kernels-only encoder sets when it opens, for a graphics list, and null for any other.
        void setCarry(MTL::Fence* fence, std::atomic<bool>* kernelsRan)
        {
            m_carryFence = fence;
            m_kernelsRan = kernelsRan;
        }

        /// Sets the stages the encoders of the list's queue run, at the list's acquisition: compute and render stages on a
        /// graphics list, `kComputeStages` on a compute list and `MTL::StageBlit` on a copy list, whose commands are all blits.
        void setQueueStages(MTL::Stages stages)
        {
            m_queueStages = stages;
        }

        /// Returns the stages the encoders of the list's queue run.
        MTL::Stages queueStages() const
        {
            return m_queueStages;
        }

        /// Returns the carry fence the list updates and waits on, or null when it uses none.
        MTL::Fence* carryFence() const
        {
            return m_carryFence;
        }

        /// Returns true while a render pass's ended encoder stays open.
        bool lingering() const
        {
            return m_lingering;
        }

        /// Returns true when the open compute encoder has waited on the carry fence.
        bool carryWaited() const
        {
            return m_carryWaited;
        }

        /// Notes that the open encoder holds work.
        void markWork()
        {
            m_holdsWork = true;
        }

        /// Returns true when an encoder is open and holds work.
        bool holdsWork() const
        {
            return m_holdsWork;
        }

        /// Returns the kind of the open encoder.
        EncoderKind kind() const
        {
            return m_kind;
        }

        /// Returns the open compute encoder, or null when none is open.
        MTL4::ComputeCommandEncoder* computeEncoder() const
        {
            return m_compute;
        }

        /// Returns the open render encoder, or null when none is open or it lingers after its pass.
        MTL4::RenderCommandEncoder* renderEncoder() const
        {
            return m_lingering ? nullptr : m_render;
        }

        /// Returns the stages of the open encoder; none when no encoder is open.
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

        /// Returns the compute pipeline set on the open compute encoder, a backend kernel's or the caller's, or null when none
        /// is set.
        const MTL::ComputePipelineState* kernelPipeline() const
        {
            return m_kernelPipeline;
        }

        /// Records that `pipeline` is set on the open compute encoder.
        void setKernelPipeline(const MTL::ComputePipelineState* pipeline)
        {
            m_kernelPipeline = pipeline;
        }

        /// Returns what the list owes of its barriers and what the open encoder's opening barrier covered.
        BarrierState& barriers()
        {
            return m_barriers;
        }

        /// Returns what the list owes of its barriers and what the open encoder's opening barrier covered.
        const BarrierState& barriers() const
        {
            return m_barriers;
        }

        /// Ends the open encoder, counting its `endEncoding` on `list`, and keeps it for `releaseEnded`; an encoder-level
        /// barrier still held back joins the pending barrier, as do the stages its opening barrier held back in which it ran no
        /// command (for a render encoder, those `markRan` did not name besides the fragment stage); what that barrier covered,
        /// and the kernel pipeline set on it, are then forgotten. A render encoder ends inside an autorelease pool, because the
        /// validation layer autoreleases there. A lingering encoder added those stages when it began to linger. Does nothing
        /// when none is open.
        void close(core::CommandListCore& list);

        /// Releases an encoder still open without ending it, releases the ended ones (`releaseEnded`) and forgets every
        /// barrier: a list that never ended, which only a Release build lets through, leaves them behind. It opens an
        /// autorelease pool only when there is one to release.
        void release();

        /// Releases the encoders `close` ended, which the list keeps retained until its frame slot is reused, as an
        /// autorelease pool drained once per frame would, so that no encoder is freed inside a recording. The `frameSlot` hook
        /// calls it after the frame wait proved their work done; the caller holds an autorelease pool.
        void releaseEnded();

        /// Returns how many ended encoders the list keeps until its slot is reused.
        size_t endedCount() const
        {
            return m_ended.size();
        }

      private:
        /// `compute` for every case its inline test does not take: opens the encoder, or emits what is owed before the command.
        MTL4::ComputeCommandEncoder* prepareCompute(core::CommandListCore& list, ListState& state, MTL::Stages command, bool kernelsOnly);

        /// Returns true when the open compute encoder still owes the carry fence's wait before a command in stage `command`:
        /// the conditions `waitCarry` skips the wait on.
        bool owesCarryWait(MTL::Stages command) const
        {
            return !m_carryWaited && !m_kernelsOnly && m_carryFence != nullptr && (command & MTL::StageDispatch) != 0;
        }

        /// Moves into the pending barrier what the open encoder leaves owed when it runs no further command: the
        /// encoder-level barrier held back and the stages its opening barrier held in which it ran no command.
        void settle();

        /// Returns the stages the open encoder ran a command in.
        MTL::Stages ranStages() const;

        /// Waits on the carry fence before a command in stage `command` on the open compute encoder, unless the encoder waited
        /// already, runs only backend kernels, the command is not in the dispatch stage or the list uses no carry fence;
        /// counts the wait on `list`.
        void waitCarry(core::CommandListCore& list, MTL::Stages command);

        MTL4::ComputeCommandEncoder* m_compute = nullptr; ///< The open compute encoder, retained; null when none is open.
        MTL4::RenderCommandEncoder* m_render = nullptr;   ///< The open render encoder, retained; null when none is open.
        EncoderKind m_kind = EncoderKind::None;           ///< The kind of the open encoder.
        bool m_holdsWork = false;                         ///< The open encoder holds at least one command.
        bool m_commandPools = false;                      ///< The list's `commandPools`, taken when an encoder opens.
        MTL::Stages m_ran = 0;                            ///< The stages the open render encoder ran a command in.
        bool m_lingering = false;                         ///< The open render encoder's pass ended; it runs no further command.
        bool m_kernelsOnly = false;                       ///< The open compute encoder was opened to run only backend kernels.
        bool m_carryWaited = false;                       ///< The open compute encoder waited on the carry fence.
        bool m_syncsAtEnd = false;                        ///< The open encoder carried a barrier to later lists at `end`.
        MTL::Fence* m_carryFence = nullptr;               ///< The carry fence the list uses; null but on a fenced graphics list.
        std::atomic<bool>* m_kernelsRan = nullptr;        ///< The graphics queue's flag a kernels-only encoder sets; null on other lists.
        MTL::Stages m_queueStages = 0;                    ///< The stages the encoders of the list's queue run.
        BarrierState m_barriers;                          ///< The pending barrier and the open encoder's covered stages.
        LastGlobal m_lastGlobal;                          ///< The last batch of one global barrier lowered; kept across recordings.
        /// The compute pipeline last set on the open compute encoder, a backend kernel's or the caller's, so neither is set
        /// again while the encoder holds it, which the validation layer reports as redundant. Reset when an encoder opens or
        /// closes. Every entry that sets a compute pipeline on the encoder records it with `setKernelPipeline`.
        const MTL::ComputePipelineState* m_kernelPipeline = nullptr;
        /// The encoders `close` ended, each still retained, until `releaseEnded`; the vector keeps its capacity.
        std::vector<MTL4::CommandEncoder*> m_ended;
    };
} // namespace rojoRHI::metal4
