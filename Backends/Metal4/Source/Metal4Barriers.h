//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Barriers.h
/// @brief Declares the lowering of barrier batches to Metal 4 stage barriers: the stage table, the pending queue-level barrier and the forms the encoders emit.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Metal4ListState.h"

#include <Metal/Metal.hpp>

#include <cstring>

namespace rojoRHI::metal4 {
    // A compute encoder records dispatches, copies and fills.
    inline constexpr MTL::Stages kComputeStages = MTL::StageDispatch | MTL::StageBlit;

    inline constexpr MTL::Stages kRenderStages = MTL::StageVertex | MTL::StageObject | MTL::StageMesh | MTL::StageFragment;

    // The union of chapter 09's row for each bit, with `Copy` as blit, dispatch and fragment work.
    MTL::Stages metalStages(Stage stages);

    struct BatchLowering
    {
        MTL::Stages source = 0;
        MTL::Stages destination = 0;
        MTL4::VisibilityOptions visibility = 0; // `Device` when any access is named, `ResourceAlias` for an alias.
        bool args = false;
    };

    // The lowering omits the alias visibility, which depends on the device's memory rather than on the record. The next
    // batch of one global barrier reuses it when it names the same record, as a list's batches mostly do. The initial
    // record lowers to nothing, which the initial lowering holds.
    struct LastGlobal
    {
        GlobalBarrier record;
        BatchLowering lowering;

        bool holds(const GlobalBarrier& global) const
        {
            // The record is four 32-bit fields with no padding, so its bytes compare as its fields do.
            return std::memcmp(&global, &record, sizeof(GlobalBarrier)) == 0;
        }
    };

    // A pending barrier waits at queue level for its source stages before the destination stages it still names; each
    // encoder that opens emits the part on its own stages, and the last or a trailing encoder carries the rest at
    // `CommandList::end`. An encoder-level barrier is held back until the open encoder's next command in a remaining
    // destination stage; what remains joins the pending barrier if the encoder closes first.
    struct BarrierState
    {
        MTL::Stages pendingSource = 0;
        MTL::Stages pendingDestination = 0; // Not yet emitted by any encoder; empty when none is pending.
        MTL4::VisibilityOptions pendingVisibility = 0;
        MTL::Stages openSource = 0;      // Empty when the opening emitted none.
        MTL::Stages openDestination = 0; // The open encoder's stages its opening barrier held back; empty when it emitted none.
        MTL4::VisibilityOptions openVisibility = 0;
        // The stages a compute encoder had run a command in when `barrierInOpen` made its barrier the opening one, and has
        // run none in since: the queue-level part orders only commands the encoder encodes after it, so these stages
        // still count as unrun for the encoder's end. Empty for an opening barrier emitted before any command.
        MTL::Stages preRan = 0;
        // The stages and visibility two opening barriers of one encoder held back, which `openSource`, `openDestination` and
        // `openVisibility` do not stand for: the unions of two barriers order less than each does. They only return to the
        // pending barrier with the stages no command ran in.
        MTL::Stages mergedSource = 0;
        MTL::Stages mergedDestination = 0;
        MTL4::VisibilityOptions mergedVisibility = 0;
        MTL::Stages deferredSource = 0;      // Held back until a command in one of its stages.
        MTL::Stages deferredDestination = 0; // All of the open encoder's; empty when none is held back.
        MTL4::VisibilityOptions deferredVisibility = 0;
        // What the open compute encoder's own commands spent of the barriers it kept: the sources it ran in, the destination
        // stages a command followed in, and their visibility. Nothing later orders that work, so the encoder ends with a
        // producer barrier for it.
        MTL::Stages keptSource = 0;
        MTL::Stages keptDestination = 0;
        MTL4::VisibilityOptions keptVisibility = 0;
        // MetalFX's encoders update it when they finish. The first compute and the first render encoder after the
        // upscale each wait on it before their opening barrier, and the trailing encoder waits on it at
        // `CommandList::end`.
        MTL::Fence* owedFence = nullptr;
        // `kComputeStages`, `kRenderStages` or both.
        MTL::Stages owedFenceStages = 0;
        // Set when a barrier names `Stage::IndirectArgs` or `Stage::All` as a destination: what the list owes at its
        // end may then be records a counted draw's kernels read, so the dispatch stage is held back with a barrier, not
        // the carry fence those kernels do not wait on.
        bool argsNamed = false;

        void merge(MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility)
        {
            pendingSource |= source;
            pendingDestination |= destination;
            pendingVisibility |= visibility;
        }

        // Merges with a barrier already held back.
        void defer(MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility)
        {
            deferredSource |= source;
            deferredDestination |= destination;
            deferredVisibility |= visibility;
        }

        // An encoder-level barrier orders only later commands of its own encoder.
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

        // That barrier ordered only the encoder's own work, so a later encoder that runs the unrun stages still needs
        // it.
        void returnUnrun(MTL::Stages ran)
        {
            const MTL::Stages unrun = (openDestination | mergedDestination) & ~ran;
            if (unrun != 0)
            {
                merge(openSource | mergedSource, unrun, openVisibility | mergedVisibility);
            }
        }

        // The encoder's opening barrier meets one emitted before its first command: neither stands for both.
        void mergeOpened(MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility)
        {
            mergedSource |= source | openSource;
            mergedDestination |= destination | openDestination;
            mergedVisibility |= visibility | openVisibility;
            openSource = 0;
            openDestination = 0;
            openVisibility = 0;
        }

        void keep(MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility)
        {
            keptSource |= source;
            keptDestination |= destination;
            keptVisibility |= visibility;
        }

        // Whether a barrier from `source` to `destination` with `visibility` orders what the encoder, which ran `ran`,
        // spent of its kept ones.
        bool keptWithin(MTL::Stages ran, MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility) const
        {
            return (keptSource & ran & ~source) == 0 && (keptDestination & ~destination) == 0 && (keptVisibility & ~visibility) == 0;
        }

        void clearKept()
        {
            keptSource = 0;
            keptDestination = 0;
            keptVisibility = 0;
        }

        void clearOpen()
        {
            openSource = 0;
            openDestination = 0;
            openVisibility = 0;
            preRan = 0;
            mergedSource = 0;
            mergedDestination = 0;
            mergedVisibility = 0;
        }

        void reset()
        {
            *this = {};
        }
    };

    // The dispatch stage among `destination` when the list uses the carry fence, no barrier of the list named
    // `IndirectArgs` as a destination, and `visibility` has no `ResourceAlias`, which a fence cannot flush; else none.
    inline MTL::Stages fencedStages(const BarrierState& barriers, const MTL::Fence* carry, MTL::Stages destination, MTL4::VisibilityOptions visibility)
    {
        const bool fenced = carry != nullptr && !barriers.argsNamed && (visibility & MTL4::VisibilityOptionResourceAlias) == 0;
        return fenced ? destination & MTL::StageDispatch : MTL::Stages{ 0 };
    }

    // The same for the pending barrier.
    inline MTL::Stages fencedStages(const BarrierState& barriers, const MTL::Fence* carry)
    {
        return fencedStages(barriers, carry, barriers.pendingDestination, barriers.pendingVisibility);
    }

    // The upscale's fence wait comes first, for every stage of the encoder's kind (a render encoder's `stages` may be
    // its fragment stage alone), then the pending barrier's part on `stages` as a queue-level barrier.
    void emitAtOpen(core::CommandListCore& list, BarrierState& barriers, MTL4::CommandEncoder* encoder, MTL::Stages stages);

    // Its sources outside the encoder were covered by the encoder's opening barrier, so only those on its stages need
    // it. Its other destination stages stay held.
    void emitDeferred(core::CommandListCore& list, BarrierState& barriers, MTL4::CommandEncoder* encoder, MTL::Stages stages, MTL::Stages command);

    // On a compute encoder that holds no command: a queue-level barrier from the pending sources to its stages, an
    // update of `carry` for the stages `fencedStages` names, then a barrier from its stages to the destinations left.
    // After an upscale it first waits on the upscale's fence and holds back every later stage, so held-back work
    // follows MetalFX's. `carry` is null on a list that uses no carry fence, every compute and copy list among them.
    void emitTrailing(core::CommandListCore& list, ListState& state, BarrierState& barriers, MTL::Fence* carry);
} // namespace rojoRHI::metal4
