//----------------------------------------------------------------------------------------------------------------------
/// @file Aliasing.h
/// @brief Declares the heap state the validation tracker keeps of the placed resources that last used each byte range.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/QueueType.h>

#include "Core/Tracker/Hazards.h"
#include "Core/Tracker/OffsetMap.h"

#include <array>
#include <cstdint>
#include <optional>

namespace rojoRHI::core {
    /// The placed resource that made a use of heap memory: a buffer or a texture, by handle bits.
    struct HeapOwner
    {
        uint64_t bits = 0;    ///< The handle bits of the resource.
        bool texture = false; ///< The resource is a texture; otherwise a buffer.

        bool operator==(const HeapOwner&) const = default;
    };

    /// The last use of some heap bytes on one queue: the placed resource that made it, and the use with the global barriers
    /// replayed on the queue since it, which alone order a placed buffer's first use after it (VU-rojo-sync-0009).
    struct HeapUse
    {
        HeapOwner owner;    ///< The resource that made the use.
        TrackedUse use;     ///< The use: its position, its stage and access, and the global barriers since.
        uint64_t stamp = 0; ///< The order of the use among the heap uses replayed on every queue.

        bool operator==(const HeapUse&) const = default;
    };

    /// The last use of some heap bytes on each queue type, indexed by `QueueType`.
    using HeapUses = std::array<std::optional<HeapUse>, 3>;

    /// The bytes of one heap, each range with its last use on every queue.
    using HeapSegments = OffsetMap<HeapUses>;

    /// The latest use another resource made of some bytes of a placed texture subresource's memory on one queue since the
    /// subresource's last event there, which its first event there since must follow: a texture barrier from
    /// `Layout::Undefined` whose source covers it (VU-rojo-sync-0017). A newer use of the same bytes replaces it; whether the
    /// earlier uses were ordered before the newer one is those resources' own rules' to judge.
    struct AliasMark
    {
        HeapOwner owner;       ///< The resource that made the use.
        uint64_t position = 0; ///< The use's position on the queue; uses of one command by one resource merge.
        uint64_t stamp = 0;    ///< The use's order among the heap uses of every queue.
        Scope scope;           ///< The use's stage and access.

        bool operator==(const AliasMark&) const = default;
    };

    /// The marks of one texture subresource on one queue, by heap byte range: as many as the distinct byte ranges other
    /// resources used last.
    using AliasMarks = OffsetMap<std::optional<AliasMark>>;

    /// Notes on `uses`, the last uses of some heap bytes, the use `scope`, at `position` on `queue` and `stamp` among the heap
    /// uses of every queue, by `owner`: it becomes their last use there, merged into the remembered one when the same command
    /// of the same resource made both.
    void noteHeapUse(HeapUses& uses, QueueType queue, HeapOwner owner, uint64_t position, uint64_t stamp, Scope scope);

    /// Notes a global barrier recorded at `position` from `source` to `destination` over the last use in `uses` on `queue`,
    /// when that use comes before it.
    void reachHeapUse(HeapUses& uses, QueueType queue, uint64_t position, Scope source, Scope destination);

    /// Returns true when a global barrier since `other`, another resource's last use of the bytes, orders `use`, a placed
    /// buffer's first use of them since (VU-rojo-sync-0009): its source covers `other` and its destination covers `use`.
    bool aliasOrdered(const HeapUse& other, Scope use);

    /// Notes on `mark`, the latest use another resource made of some bytes of a texture subresource's memory on one queue
    /// since its last event there, the use `scope` by `owner` at `position` and `stamp`: it merges into a use of the same
    /// command by the same resource, replaces an older use and leaves a newer one.
    void markAlias(std::optional<AliasMark>& mark, HeapOwner owner, uint64_t position, uint64_t stamp, Scope scope);

    /// Notes on `marks` the use `scope` by `owner` at `position` and `stamp` of the heap bytes `begin` to `end`, as `markAlias`
    /// does for each range of them.
    void markAlias(AliasMarks& marks, uint64_t begin, uint64_t end, HeapOwner owner, uint64_t position, uint64_t stamp, Scope scope);
} // namespace rojoRHI::core
