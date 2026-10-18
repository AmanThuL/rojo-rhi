//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Barriers.h
/// @brief Declares the lowering of barrier batches to Metal 4 stage barriers: the stage table, the pending queue-level barrier and the forms the encoders emit.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Metal4ListState.h"

#include <Metal/Metal.hpp>

namespace rojoRHI::metal4 {
    /// The stages of a compute encoder, which records dispatches, copies and fills.
    inline constexpr MTL::Stages kComputeStages = MTL::StageDispatch | MTL::StageBlit;

    /// The stages of a render encoder.
    inline constexpr MTL::Stages kRenderStages = MTL::StageVertex | MTL::StageObject | MTL::StageMesh | MTL::StageFragment;

    /// Returns the Metal stages `stages` lowers to: the union of chapter 09's row for each bit, with `Copy` as blit, dispatch
    /// and fragment work and `All` as every Metal stage.
    MTL::Stages metalStages(Stage stages);

    /// What a list still owes of its barriers, and what the open encoder's opening barrier covered. A pending barrier waits at
    /// queue level for its source stages before the destination stages it still names; each encoder that opens emits the part
    /// of it that falls on its own stages, and a closing encoder emits the rest at `CommandList::end`. An encoder-level barrier
    /// is held back until the open encoder's next command, and joins the pending barrier if the encoder closes first.
    struct BarrierState
    {
        MTL::Stages pendingSource = 0;                  ///< The source stages of the pending barrier.
        MTL::Stages pendingDestination = 0;             ///< The destination stages no encoder has emitted yet; empty when none is pending.
        MTL4::VisibilityOptions pendingVisibility = 0;  ///< The visibility of the pending barrier.
        MTL::Stages openSource = 0;                     ///< The source stages the open encoder's opening barrier waited for; empty when it emitted none.
        MTL::Stages openDestination = 0;                ///< The open encoder's stages its opening barrier held back; empty when it emitted none.
        MTL4::VisibilityOptions openVisibility = 0;     ///< The visibility of the open encoder's opening barrier.
        MTL::Stages deferredSource = 0;                 ///< The source stages of the encoder-level barrier held back until the encoder's next command.
        MTL::Stages deferredDestination = 0;            ///< Its destination stages, all of the open encoder's; empty when none is held back.
        MTL4::VisibilityOptions deferredVisibility = 0; ///< Its visibility.

        /// Adds a barrier from `source` to `destination` with `visibility` to the pending barrier.
        void merge(MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility)
        {
            pendingSource |= source;
            pendingDestination |= destination;
            pendingVisibility |= visibility;
        }

        /// Holds back an encoder-level barrier from `source` to `destination`, the open encoder's stages, with `visibility`,
        /// merging it with one already held back.
        void defer(MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility)
        {
            deferredSource |= source;
            deferredDestination |= destination;
            deferredVisibility |= visibility;
        }

        /// Moves the barrier held back into the pending barrier, as when its encoder closes before any command follows it: an
        /// encoder-level barrier orders only later commands of its own encoder.
        void queueDeferred()
        {
            if (deferredDestination != 0)
            {
                merge(deferredSource, deferredDestination, deferredVisibility);
            }
            deferredSource = 0;
            deferredDestination = 0;
            deferredVisibility = 0;
        }

        /// Returns to the pending barrier the stages the closing encoder's opening barrier held back but in which the encoder
        /// ran no command, `ran` naming those it did: that barrier ordered only the encoder's own work, so a later encoder that
        /// runs them still needs it.
        void returnUnrun(MTL::Stages ran)
        {
            const MTL::Stages unrun = openDestination & ~ran;
            if (unrun != 0)
            {
                merge(openSource, unrun, openVisibility);
            }
        }

        /// Forgets what the open encoder's opening barrier covered, as when that encoder closes.
        void clearOpen()
        {
            openSource = 0;
            openDestination = 0;
            openVisibility = 0;
        }

        /// Forgets everything, as when the list is acquired again.
        void reset()
        {
            *this = {};
        }
    };

    /// Emits, on `encoder`, which has just opened with `stages`, the part of the pending barrier of `barriers` that falls on
    /// those stages as a queue-level barrier, and records what it covered; counts the barrier on `list`. Records that the
    /// encoder's opening covered nothing when no part falls on its stages.
    void emitAtOpen(core::CommandListCore& list, BarrierState& barriers, MTL4::CommandEncoder* encoder, MTL::Stages stages);

    /// Emits on `encoder`, open with `stages`, the encoder-level barrier held back, before a command about to be recorded in
    /// stage `command`; counts it on `list`. Its sources outside the encoder were covered by the encoder's opening barrier, so
    /// only those on its stages need it. Does nothing when no barrier is held back or it does not hold back `command`.
    void emitDeferred(core::CommandListCore& list, BarrierState& barriers, MTL4::CommandEncoder* encoder, MTL::Stages stages, MTL::Stages command);

    /// Emits the barrier still pending at `CommandList::end` on a compute encoder that holds no command: a queue-level barrier
    /// from the pending source stages to its own, then one from its own stages to the destination stages still owed, which
    /// holds back every later encoder on the queue, in this list or a later one. Counts the encoder, its five native calls
    /// and two barriers on `list`; does nothing when no barrier is pending.
    void emitTrailing(core::CommandListCore& list, ListState& state, BarrierState& barriers);
} // namespace rojoRHI::metal4
