//----------------------------------------------------------------------------------------------------------------------
/// @file Aliasing.cpp
/// @brief Implements the heap state the validation tracker keeps of the placed resources that last used each byte range.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Tracker/Aliasing.h"

#include <algorithm>
#include <cstddef>

namespace rojoRHI::core {
    //==================================================================================================================
    void noteHeapUse(HeapUses& uses, QueueType queue, HeapOwner owner, uint64_t position, uint64_t stamp, Scope scope)
    {
        std::optional<HeapUse>& last = uses[static_cast<size_t>(queue)];
        if (last.has_value() && last->owner == owner && last->use.position == position)
        {
            last->use.scope = Scope{ .stage = last->use.scope.stage | scope.stage, .access = last->use.scope.access | scope.access };
            last->stamp = stamp;
            return;
        }
        last = HeapUse{ .owner = owner, .use = TrackedUse{ .position = position, .scope = scope }, .stamp = stamp };
    }

    //==================================================================================================================
    void reachHeapUse(HeapUses& uses, QueueType queue, uint64_t position, Scope source, Scope destination)
    {
        std::optional<HeapUse>& last = uses[static_cast<size_t>(queue)];
        if (!last.has_value() || last->use.position >= position)
        {
            return;
        }
        // The first barrier of each kind is kept: an earlier one orders whatever a later one of its kind would.
        const Reach added{ .destination = destination, .sourceCovers = sourceCovers(source, last->use.scope), .position = position };
        const bool known = std::ranges::any_of(last->use.reaches, [&](const Reach& reach) {
            return reach.destination == added.destination && reach.sourceCovers == added.sourceCovers;
        });
        if (!known)
        {
            last->use.reaches.push_back(added);
        }
    }

    //==================================================================================================================
    bool aliasOrdered(const HeapUse& other, Scope use)
    {
        const bool writes = hasWrite(other.use.scope.access);
        return std::ranges::any_of(other.use.reaches, [&](const Reach& reach) {
            return reach.sourceCovers && destinationCovers(reach.destination, use, writes);
        });
    }

    //==================================================================================================================
    void markAlias(std::optional<AliasMark>& mark, HeapOwner owner, uint64_t position, uint64_t stamp, Scope scope)
    {
        if (mark.has_value() && mark->owner == owner && mark->position == position)
        {
            mark->stamp = std::max(mark->stamp, stamp);
            mark->scope = Scope{ .stage = mark->scope.stage | scope.stage, .access = mark->scope.access | scope.access };
            return;
        }
        if (!mark.has_value() || mark->stamp < stamp)
        {
            mark = AliasMark{ .owner = owner, .position = position, .stamp = stamp, .scope = scope };
        }
    }

    //==================================================================================================================
    void markAlias(AliasMarks& marks, uint64_t begin, uint64_t end, HeapOwner owner, uint64_t position, uint64_t stamp, Scope scope)
    {
        for (AliasMarks::Range& range : marks.isolate(begin, end))
        {
            markAlias(range.state, owner, position, stamp, scope);
        }
        marks.merge(begin, end);
    }
} // namespace rojoRHI::core
