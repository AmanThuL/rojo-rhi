//----------------------------------------------------------------------------------------------------------------------
/// @file Aliasing.h
/// @brief Declares the heap state the validation tracker keeps of the placed resources that last used each byte range.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Tracker/Hazards.h"
#include "Core/Tracker/OffsetMap.h"

#include <array>
#include <cstdint>
#include <optional>

namespace rojoRHI::core {
    struct HeapOwner
    {
        uint64_t bits = 0;
        bool texture = false;

        bool operator==(const HeapOwner&) const = default;
    };

    // Only the global barriers replayed on the queue since the use order a placed buffer's first use after it
    // (VU-rojo-sync-0009); its position tells whether a clock covers it (VU-rojo-queue-0005).
    struct HeapUse
    {
        HeapOwner owner;
        TrackedUse use;

        bool operator==(const HeapUse&) const = default;
    };

    // Indexed by `QueueType`.
    using HeapUses = std::array<std::optional<HeapUse>, 3>;

    using HeapSegments = OffsetMap<HeapUses>;

    // Another resource's latest use of a placed texture subresource's bytes on one queue since the subresource's last event
    // there. The subresource's next first event must be a barrier from `Layout::Undefined` whose source covers it, unless a
    // clock covers it (VU-rojo-sync-0017). A newer use replaces it: ordering among the others is their own rules' to judge.
    struct AliasMark
    {
        HeapOwner owner;
        uint64_t position = 0;
        Scope scope;

        bool operator==(const AliasMark&) const = default;
    };

    using AliasMarks = OffsetMap<std::optional<AliasMark>>;

    // Merges into the last use when the same command of the same resource made both. A `transition` (a placed texture's
    // barrier that changes the layout or discards) passes its destination as `scope` and counts as a write whatever its
    // accesses.
    void noteHeapUse(HeapUses& uses, QueueType queue, HeapOwner owner, uint64_t position, Scope scope, bool transition = false);

    void reachHeapUse(HeapUses& uses, QueueType queue, uint64_t position, Scope source, Scope destination);

    // VU-rojo-sync-0009: a global barrier since `other` whose source covers it and whose destination covers `use`, all its
    // accesses when `other` writes or is a transition.
    bool aliasOrdered(const HeapUse& other, Scope use);

    void markAlias(std::optional<AliasMark>& mark, HeapOwner owner, uint64_t position, Scope scope);

    void markAlias(AliasMarks& marks, uint64_t begin, uint64_t end, HeapOwner owner, uint64_t position, Scope scope);

    // A mark that `ordered` covers owes no discard: the event the clock orders after it is the first since it.
    void clearCovered(AliasMarks& marks, uint64_t begin, uint64_t end, QueueType queue, const Clock& ordered);
} // namespace rojoRHI::core
