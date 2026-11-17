//----------------------------------------------------------------------------------------------------------------------
/// @file Hazards.h
/// @brief Declares the hazard state the validation tracker keeps per unit of memory and queue, and the coverage rules over it.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Tracker/Clocks.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rojoRHI::core {
    struct Scope
    {
        Stage stage = Stage::None;
        Access access = Access::None;

        bool operator==(const Scope&) const = default;
    };

    // VU-rojo-sync-0024: every stage bit of the use, and every write access of it, since a read needs no availability. A
    // discard from `Layout::Undefined` needs no source access. The bit test lowers a combined stage such as `All` as chapter
    // 09 does. A source with no stage covers no use.
    bool sourceCovers(Scope source, Scope use, bool discard = false);

    // VU-rojo-sync-0024: every stage bit of the use and, after a write, every access of it, which must see the write.
    bool destinationCovers(Scope destination, Scope use, bool earlierWrites);

    // Sharing a stage is what lets an execution dependency chain from one barrier into another.
    bool sharesStage(Stage a, Stage b);

    bool hasWrite(Access access);

    // A barrier replayed after a use, on its queue and over its memory.
    struct Reach
    {
        Scope destination;
        // The source covers the use, or chains over it by sharing a stage with the destination of an earlier reach that orders
        // it. A layout transition's own destination is its first reach, so a later barrier orders it only through a chain.
        bool sourceCovers = false;
        // Of the first barrier of its kind, since an earlier one orders whatever a later one would. A reach at a transition's
        // own position, a record of its own barrier call, never orders it.
        uint64_t position = 0;

        bool operator==(const Reach&) const = default;
    };

    struct TrackedUse
    {
        // Uses of one command share a position and are never compared with each other; the records of one barrier call share
        // one too, so they never order each other.
        uint64_t position = 0;
        Scope scope; // For a transition, its destination.
        // A texture barrier that changed the layout, standing as a write between the uses around it.
        bool transition = false;
        // For a read: a barrier ordered it after the last write, so a write after the read may be ordered after that write
        // through it.
        bool afterWrite = false;
        // Each once, in no meaningful order.
        std::vector<Reach> reaches;

        bool operator==(const TrackedUse&) const = default;
    };

    enum class HazardKind : uint8_t
    {
        None,
        NoBarrier, // No barrier lies between a pair that needs one (VU-rojo-sync-0021).
        Unordered  // Barriers lie between such a pair, and none orders it (VU-rojo-sync-0024).
    };

    struct Hazard
    {
        HazardKind kind = HazardKind::None;
        Scope earlier; // A transition's destination when `earlierTransition`.
        bool earlierTransition = false;
    };

    // Read by a check that reports its own rule ahead of VU-rojo-sync-0021 and VU-rojo-sync-0024.
    enum class WriteOrder : uint8_t
    {
        None,      // The queue has no write of the memory to order.
        Ordered,   // A barrier since the write covers the write at its source and the use at its destination.
        Unordered, // A barrier since the write has a destination that covers the use, but none of those covers the write.
        Missing    // No barrier since the write has a destination that covers the use.
    };

    // Uses on different queues are never compared.
    struct QueueHazards
    {
        std::optional<TrackedUse> write; // Or the last layout transition.
        std::vector<TrackedUse> reads;   // Since `write`, or since tracking began, in replay order.

        bool operator==(const QueueHazards&) const = default;
    };

    // One texture subresource or buffer segment.
    struct UnitHazards
    {
        std::array<QueueHazards, 3> queues;

        QueueHazards& on(QueueType queue)
        {
            return queues[static_cast<size_t>(queue)];
        }

        bool operator==(const UnitHazards&) const = default;
    };

    // VU-rojo-sync-0021, VU-rojo-sync-0024: a read against the last write, a write against the reads since it and the write.
    // A pair is ordered by an ordering reach of the earlier use whose destination covers the later one; a write after a write
    // also through a read between them ordered after the first and before the second. A use at or below `fenced` is ordered by
    // a fence wait or the CPU's observation, with full visibility. Returns the latest unordered pair; uses at `position` are
    // the same command's and are not judged.
    Hazard checkUse(const QueueHazards& state, uint64_t position, Scope use, uint64_t fenced = 0);

    // `fenced` is the one `checkUse` judged with. A use at a remembered one's position, the same command's, merges into it.
    void recordUse(QueueHazards& state, uint64_t position, Scope use, uint64_t fenced = 0);

    // Each remembered use before `position` gains a reach, which orders it when `source` covers it or chains over it from a
    // reach of an earlier barrier call.
    void recordBarrier(QueueHazards& state, uint64_t position, Scope source, Scope destination);

    // A transition is a write ordered after earlier uses only through its own source or an execution chain: it shares a stage
    // with the destination of a reach that orders the use, an earlier transition's own included; the last write may also be
    // ordered through a read a barrier ordered after it. A record of the same barrier call never orders it, and a source with
    // no stage orders nothing. Returns the latest unordered use as `HazardKind::Unordered`, since the transition is itself a
    // barrier between.
    Hazard checkTransition(const QueueHazards& state, uint64_t position, Scope source, bool discard, uint64_t fenced = 0);

    // The transition becomes the write, the reads are forgotten, and its own destination is its first reach.
    void recordTransition(QueueHazards& state, uint64_t position, Scope destination);

    struct QueueHazard
    {
        QueueType queue = QueueType::Graphics;
        Scope earlier; // A transition's destination when `earlierTransition`.
        bool earlierTransition = false;
    };

    // VU-rojo-queue-0005: a pair on different queues, at least one of which writes, is ordered only when `ordered`, the later
    // submission's clock joined with the host clock, covers the earlier use. A `transition` is a write whatever its accesses.
    std::optional<QueueHazard> checkOtherQueues(const UnitHazards& unit, QueueType queue, Scope use, const Clock& ordered, bool transition = false);

    // A write at or below `fenced` is ordered by a fence wait or the CPU's observation.
    WriteOrder lastWriteOrder(const QueueHazards& state, Scope use, uint64_t fenced = 0);

    // Alike for every later use, transition and barrier: writes and reads match in all but position, reaches compared as sets,
    // and every position is below `floor` of its queue type, so no later use is of the same command.
    bool mergeable(const UnitHazards& a, const UnitHazards& b, const std::array<uint64_t, 3>& floor);
} // namespace rojoRHI::core
