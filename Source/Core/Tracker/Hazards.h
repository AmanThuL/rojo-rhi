//----------------------------------------------------------------------------------------------------------------------
/// @file Hazards.h
/// @brief Declares the hazard state the validation tracker keeps per unit of memory and queue, and the coverage rules over it.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Access.h>
#include <rojoRHI/QueueType.h>
#include <rojoRHI/Stage.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rojoRHI::core {
    /// A stage set and an access set: what a use makes, or one side of a barrier.
    struct Scope
    {
        Stage stage = Stage::None;    ///< The stages.
        Access access = Access::None; ///< The accesses.

        bool operator==(const Scope&) const = default;
    };

    /// Returns true when `source`, the source side of a barrier, covers `use` (VU-rojo-sync-0024): its stage set includes every
    /// stage bit of the use and its access set every write access of the use, since a read needs no availability. A barrier
    /// from `Layout::Undefined` (`discard`) needs no source access. A combined stage such as `AllGraphics` or `All` is the
    /// union of its members' bits, so the bit test lowers it as chapter 09 does. A source with no stage covers no use.
    bool sourceCovers(Scope source, Scope use, bool discard = false);

    /// Returns true when `destination`, the destination side of a barrier, covers `use` after an earlier use that writes when
    /// `earlierWrites` (VU-rojo-sync-0024): its stage set includes every stage bit of the use, and, after a write, its access
    /// set every access of the use, which must see the write.
    bool destinationCovers(Scope destination, Scope use, bool earlierWrites);

    /// Returns true when the stage sets `a` and `b` share a stage: an execution dependency chains from one into the other.
    bool sharesStage(Stage a, Stage b);

    /// Returns true when `access` holds a write: `ShaderWrite`, `ColorAttachmentWrite`, `DepthStencilWrite`, `CopyWrite` or
    /// `Resolve`.
    bool hasWrite(Access access);

    /// One barrier replayed after a use, on the use's queue and over its memory: the barrier's destination, and whether its
    /// source covers the use.
    struct Reach
    {
        Scope destination; ///< The barrier's destination stage and access.
        /// The barrier's source covers the use, or, for a layout transition, shares a stage with its destination, so it
        /// orders the use before its destination.
        bool sourceCovers = false;
        /// The position of the barrier on the queue: of the first barrier of its kind, since an earlier one orders whatever a
        /// later one would. A transition is never ordered by a reach at its own position, a record of its own barrier call.
        uint64_t position = 0;

        bool operator==(const Reach&) const = default;
    };

    /// A use the hazard state remembers: the last write, or a read since it.
    struct TrackedUse
    {
        /// The use's position on its queue. Uses of one command share a position and are never compared with each other; the
        /// records of one barrier call share one too, so they never order each other.
        uint64_t position = 0;
        /// The stage and access of the use; for a transition, the transition's destination.
        Scope scope;
        /// A texture barrier that changed the layout and stands as a write between the uses around it.
        bool transition = false;
        /// For a read: a barrier between the last write and the read ordered the pair, so a write after the read may be ordered
        /// after that write through the read.
        bool afterWrite = false;
        /// The barriers replayed over the use's memory since it, each once, in no meaningful order.
        std::vector<Reach> reaches;

        bool operator==(const TrackedUse&) const = default;
    };

    /// What judging a later use or transition against the remembered uses finds.
    enum class HazardKind : uint8_t
    {
        None,      ///< Every pair is ordered, or no pair needs a barrier.
        NoBarrier, ///< No barrier lies between a pair that needs one (VU-rojo-sync-0021).
        Unordered  ///< Barriers lie between such a pair, and none orders it (VU-rojo-sync-0024).
    };

    /// The result of judging a later use: what is wrong, and the earlier use of the pair.
    struct Hazard
    {
        HazardKind kind = HazardKind::None; ///< What is wrong.
        Scope earlier;                      ///< The stage and access of the earlier use; a transition's destination.
        bool earlierTransition = false;     ///< The earlier use is a transition.
    };

    /// How the last write to some memory on a queue is ordered before a later use: what a check that reports its own rule
    /// ahead of VU-rojo-sync-0021 and VU-rojo-sync-0024 reads.
    enum class WriteOrder : uint8_t
    {
        None,      ///< The queue has no write of the memory to order.
        Ordered,   ///< A barrier since the write covers the write at its source and the use at its destination.
        Unordered, ///< A barrier since the write has a destination that covers the use, but none of those covers the write.
        Missing    ///< No barrier since the write has a destination that covers the use.
    };

    /// The hazard state of one unit of memory on one queue: its last write and the reads since it, each with the barriers
    /// replayed over it since. Uses on different queues are never compared.
    struct QueueHazards
    {
        std::optional<TrackedUse> write; ///< The last write, or the last layout transition.
        std::vector<TrackedUse> reads;   ///< The reads since `write`, or since tracking began, in replay order.

        bool operator==(const QueueHazards&) const = default;
    };

    /// The hazard state of one unit of memory, a texture subresource or a buffer segment, kept per queue type.
    struct UnitHazards
    {
        std::array<QueueHazards, 3> queues; ///< The state on each queue type, indexed by `QueueType`.

        /// Returns the state on `queue`.
        QueueHazards& on(QueueType queue)
        {
            return queues[static_cast<size_t>(queue)];
        }

        bool operator==(const UnitHazards&) const = default;
    };

    /// Judges `use`, made at `position`, against the uses `state` remembers (VU-rojo-sync-0021, VU-rojo-sync-0024): a read
    /// against the last write, a write against the reads since it and the write itself. A pair is ordered by one barrier
    /// whose source covers the earlier use and whose destination covers the later one; a write after a write also by a read
    /// between them that one barrier orders after the first write and another before the second. A layout transition is
    /// covered by its own destination, or by a later barrier whose source stage shares a stage with that destination. Returns
    /// the latest pair that is not ordered. Uses at `position` belong to the same command and are not judged.
    Hazard checkUse(const QueueHazards& state, uint64_t position, Scope use);

    /// Notes `use`, made at `position` and judged by `checkUse`: a read joins the reads, and a write replaces the write and the
    /// reads. A use at the position of a remembered one, made by the same command, is merged into it.
    void recordUse(QueueHazards& state, uint64_t position, Scope use);

    /// Notes a barrier recorded at `position` from `source` to `destination` that applies to the unit: each remembered use
    /// before `position` gains its reach.
    void recordBarrier(QueueHazards& state, uint64_t position, Scope source, Scope destination);

    /// Judges a layout transition recorded at `position` from `source`, a discard from `Layout::Undefined` when `discard`,
    /// against the uses `state` remembers: it is a write ordered after the uses before it only through an execution chain.
    /// Its own source orders a use it covers, and it chains from an earlier barrier that covers a use, or from an earlier
    /// transition's destination, when it shares a stage with that destination; the last write may also be ordered through a
    /// read a barrier ordered after it. A record of the same barrier call, at `position`, never orders it, and a source with
    /// no stage orders nothing, which is right only when nothing came before. Returns the latest use it does not order as
    /// `HazardKind::Unordered`, since the transition is itself a barrier between.
    Hazard checkTransition(const QueueHazards& state, uint64_t position, Scope source, bool discard);

    /// Notes a layout transition recorded at `position` to `destination`: it becomes the write, the reads are forgotten, and
    /// its own destination is its first reach.
    void recordTransition(QueueHazards& state, uint64_t position, Scope destination);

    /// Returns how the last write `state` remembers is ordered before a use of `use`.
    WriteOrder lastWriteOrder(const QueueHazards& state, Scope use);

    /// Returns true when the units `a` and `b` are judged alike by every later use, transition and barrier, so neighbouring
    /// buffer segments in those states may merge: on each queue type their writes and reads match in all but position, with
    /// reaches compared as sets, and every position is below `floor` of its queue type, so no later use is of the same command.
    bool mergeable(const UnitHazards& a, const UnitHazards& b, const std::array<uint64_t, 3>& floor);
} // namespace rojoRHI::core
