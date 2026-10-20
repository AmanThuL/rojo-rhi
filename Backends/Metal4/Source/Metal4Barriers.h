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
    /// of it that falls on its own stages, and the list's last encoder or a trailing encoder carries the rest to later lists
    /// at `CommandList::end`. An encoder-level barrier is held back until the open encoder's next command in one of its
    /// remaining destination stages, which it emits for those stages and keeps the rest, and what remains joins the pending
    /// barrier if the encoder closes first.
    struct BarrierState
    {
        MTL::Stages pendingSource = 0;                  ///< The source stages of the pending barrier.
        MTL::Stages pendingDestination = 0;             ///< The destination stages no encoder has emitted yet; empty when none is pending.
        MTL4::VisibilityOptions pendingVisibility = 0;  ///< The visibility of the pending barrier.
        MTL::Stages openSource = 0;                     ///< The source stages the open encoder's opening barrier waited for; empty when it emitted none.
        MTL::Stages openDestination = 0;                ///< The open encoder's stages its opening barrier held back; empty when it emitted none.
        MTL4::VisibilityOptions openVisibility = 0;     ///< The visibility of the open encoder's opening barrier.
        MTL::Stages deferredSource = 0;                 ///< The source stages of the encoder-level barrier held back until a command in one of its stages.
        MTL::Stages deferredDestination = 0;            ///< Its destination stages not yet emitted, all of the open encoder's; empty when none is held back.
        MTL4::VisibilityOptions deferredVisibility = 0; ///< Its visibility.
        /// The fence of the list's last upscale, which MetalFX's encoders update when they finish; the first compute encoder
        /// and the first render encoder the list opens after it each wait on it, before their opening barrier, and the trailing
        /// encoder waits on it at `CommandList::end`. Null when the list has not upscaled.
        MTL::Fence* owedFence = nullptr;
        /// The stages of the encoder kinds that still owe a wait on `owedFence`: `kComputeStages`, `kRenderStages` or both.
        MTL::Stages owedFenceStages = 0;
        /// A barrier of the list named `Stage::IndirectArgs`, or `Stage::All`, as a destination: what the list owes at its end
        /// may then be records a counted draw's kernels read, so it holds the dispatch stage back with a barrier rather than
        /// through the carry fence, which those kernels do not wait on.
        bool argsNamed = false;

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

        /// Moves what remains of the barrier held back into the pending barrier, as when its encoder closes before a command
        /// follows it in those stages: an encoder-level barrier orders only later commands of its own encoder.
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

    /// Returns the stages of the barrier `barriers` still owes at `CommandList::end` that the carry fence `carry` carries rather
    /// than a barrier: the dispatch stage when the barrier holds it, the list's queue has a carry fence (a graphics list), no
    /// barrier of the list named `IndirectArgs` as a destination, whose records a counted draw's kernels read, and the
    /// pending visibility has no `ResourceAlias`, which a fence, carrying no visibility option, cannot flush; else none.
    inline MTL::Stages fencedStages(const BarrierState& barriers, const MTL::Fence* carry)
    {
        const bool fenced = carry != nullptr && !barriers.argsNamed && (barriers.pendingVisibility & MTL4::VisibilityOptionResourceAlias) == 0;
        return fenced ? barriers.pendingDestination & MTL::StageDispatch : MTL::Stages{ 0 };
    }

    /// Emits, on `encoder`, which has just opened with `stages`, first the wait on the fence an upscale left owed when the
    /// encoder's kind still owes it, for every stage of that kind (a render encoder's `stages` may be its fragment stage
    /// alone), then the part of the pending barrier of `barriers` that falls on those stages as a queue-level barrier, and
    /// records what it covered; counts the wait and the barrier on `list`. Records that the encoder's opening covered nothing
    /// when no part falls on its stages.
    void emitAtOpen(core::CommandListCore& list, BarrierState& barriers, MTL4::CommandEncoder* encoder, MTL::Stages stages);

    /// Emits on `encoder`, open with `stages`, the part of the encoder-level barrier held back that holds back stage
    /// `command`, before a command about to be recorded in that stage; counts it on `list`. Its sources outside the encoder
    /// were covered by the encoder's opening barrier, so only those on its stages need it. Its other destination stages stay
    /// held. Does nothing when no barrier is held back or it does not hold back `command`.
    void emitDeferred(core::CommandListCore& list, BarrierState& barriers, MTL4::CommandEncoder* encoder, MTL::Stages stages, MTL::Stages command);

    /// Emits the barrier still pending at `CommandList::end`, which the list's last encoder did not carry, on a compute
    /// encoder that holds no command: a queue-level barrier from the pending source stages to its own, then an update
    /// of `carry` after its own stages for the stages `fencedStages` names, which later dispatches wait on, and a
    /// barrier from its own stages to the destination stages left, which holds them back in every later encoder on the
    /// queue. When the list upscaled, the encoder first waits on the last upscale's fence and holds back every stage of
    /// every later encoder, with or without a barrier pending, so the work held back follows MetalFX's. Counts the
    /// encoder, its native calls, the wait and its barriers on `list`; does nothing when no barrier is pending and the
    /// list did not upscale. `carry` is null on a compute or copy list.
    void emitTrailing(core::CommandListCore& list, ListState& state, BarrierState& barriers, MTL::Fence* carry);
} // namespace rojoRHI::metal4
