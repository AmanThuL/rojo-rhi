//----------------------------------------------------------------------------------------------------------------------
/// @file Hazards.cpp
/// @brief Implements the hazard state of one unit of memory on one queue and the coverage rules over it.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Tracker/Hazards.h"

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

namespace rojoRHI::core {
    namespace {
        constexpr Access kWrites = Access::ShaderWrite | Access::ColorAttachmentWrite | Access::DepthStencilWrite | Access::CopyWrite | Access::Resolve;

        bool isWrite(const TrackedUse& earlier)
        {
            return earlier.transition || hasWrite(earlier.scope.access);
        }

        bool ordered(const TrackedUse& earlier, Scope later)
        {
            const bool writes = isWrite(earlier);
            return std::ranges::any_of(earlier.reaches, [&](const Reach& reach) {
                return reach.sourceCovers && destinationCovers(reach.destination, later, writes);
            });
        }

        Hazard unordered(const TrackedUse& earlier)
        {
            return Hazard{ .kind = earlier.reaches.empty() ? HazardKind::NoBarrier : HazardKind::Unordered,
                           .earlier = earlier.scope,
                           .earlierTransition = earlier.transition };
        }

        bool sameKind(const Reach& a, const Reach& b)
        {
            return a.destination == b.destination && a.sourceCovers == b.sourceCovers;
        }

        // A reach's position matters only when it is at least `fresh`, a barrier call still being replayed, whose records never
        // order a transition of the same call; positions below it compare alike.
        bool sameReaches(const std::vector<Reach>& a, const std::vector<Reach>& b, uint64_t fresh)
        {
            return a.size() == b.size() && std::ranges::all_of(a, [&](const Reach& reach) {
                       return std::ranges::any_of(b, [&](const Reach& other) {
                           return sameKind(reach, other) && (reach.position >= fresh) == (other.position >= fresh);
                       });
                   });
        }

        // Positions are ignored: they only tell a later use of the same command apart, and an alike earlier use with no barrier
        // since conflicts with that later use as the dropped one would.
        bool alike(const TrackedUse& a, const TrackedUse& b, uint64_t fresh)
        {
            return a.scope == b.scope && a.transition == b.transition && a.afterWrite == b.afterWrite && sameReaches(a.reaches, b.reaches, fresh);
        }

        bool mergeable(const QueueHazards& a, const QueueHazards& b, uint64_t floor)
        {
            const auto older = [floor](const TrackedUse& use) { return use.position < floor; };
            const auto contains = [floor](const std::vector<TrackedUse>& reads, const TrackedUse& read) {
                return std::ranges::any_of(reads, [&](const TrackedUse& other) { return alike(other, read, floor); });
            };
            if (a.write.has_value() != b.write.has_value() || a.reads.size() != b.reads.size())
            {
                return false;
            }
            if (a.write.has_value() && !(older(*a.write) && older(*b.write) && alike(*a.write, *b.write, floor)))
            {
                return false;
            }
            return std::ranges::all_of(a.reads, older) && std::ranges::all_of(b.reads, older) &&
                   std::ranges::all_of(a.reads, [&](const TrackedUse& read) { return contains(b.reads, read); });
        }

        Scope merged(Scope a, Scope b)
        {
            return Scope{ .stage = a.stage | b.stage, .access = a.access | b.access };
        }

        // A record of the same barrier call, at `position`, never chains.
        bool chains(const TrackedUse& use, uint64_t position, Scope source)
        {
            return std::ranges::any_of(use.reaches, [&](const Reach& reach) {
                return reach.position != position && reach.sourceCovers && sharesStage(reach.destination.stage, source.stage);
            });
        }

        // An earlier transition is ordered by sharing a stage with its destination, not by coverage.
        bool transitionOrders(Scope source, bool discard, const TrackedUse& earlier)
        {
            return earlier.transition ? sharesStage(source.stage, earlier.scope.stage) : sourceCovers(source, earlier.scope, discard);
        }
    } // namespace

    bool sourceCovers(Scope source, Scope use, bool discard)
    {
        const Access writes = use.access & kWrites;
        return source.stage != Stage::None && (source.stage & use.stage) == use.stage && (discard || (source.access & writes) == writes);
    }

    bool destinationCovers(Scope destination, Scope use, bool earlierWrites)
    {
        return (destination.stage & use.stage) == use.stage && (!earlierWrites || (destination.access & use.access) == use.access);
    }

    bool sharesStage(Stage a, Stage b)
    {
        return (a & b) != Stage::None;
    }

    bool hasWrite(Access access)
    {
        return (access & kWrites) != Access::None;
    }

    Hazard checkUse(const QueueHazards& state, uint64_t position, Scope use, uint64_t fenced)
    {
        // A fence wait or the CPU's observation orders a use with full visibility, as a barrier covering it would.
        const auto orderedBefore = [&](const TrackedUse& earlier) { return earlier.position <= fenced || ordered(earlier, use); };
        const bool otherWrite = state.write.has_value() && state.write->position != position;
        if (!hasWrite(use.access))
        {
            return otherWrite && !orderedBefore(*state.write) ? unordered(*state.write) : Hazard{};
        }
        // A write after reads: each read, latest first, then the write before them, which a read between may order.
        for (const TrackedUse& read : std::views::reverse(state.reads))
        {
            if (read.position != position && !orderedBefore(read))
            {
                return unordered(read);
            }
        }
        if (!otherWrite || orderedBefore(*state.write))
        {
            return Hazard{};
        }
        const bool chained =
            std::ranges::any_of(state.reads, [&](const TrackedUse& read) { return read.afterWrite && read.position != position && orderedBefore(read); });
        return chained ? Hazard{} : unordered(*state.write);
    }

    void recordUse(QueueHazards& state, uint64_t position, Scope use, uint64_t fenced)
    {
        if (!hasWrite(use.access))
        {
            if (state.write.has_value() && state.write->position == position)
            {
                state.write->scope = merged(state.write->scope, use);
                return;
            }
            const auto same = std::ranges::find(state.reads, position, &TrackedUse::position);
            if (same != state.reads.end())
            {
                same->scope = merged(same->scope, use);
                return;
            }
            const bool afterWrite = state.write.has_value() && (state.write->position <= fenced || ordered(*state.write, use));
            const TrackedUse read{ .position = position, .scope = use, .afterWrite = afterWrite };
            // A read alike to one remembered adds nothing, so reads repeated without a write between keep the state bounded.
            if (std::ranges::none_of(state.reads, [&](const TrackedUse& other) { return alike(other, read, position); }))
            {
                state.reads.push_back(read);
            }
            return;
        }
        // The reads and write of this command join the new write; every earlier use is ordered before it or was reported.
        Scope scope = use;
        if (state.write.has_value() && state.write->position == position)
        {
            scope = merged(scope, state.write->scope);
        }
        for (const TrackedUse& read : state.reads)
        {
            if (read.position == position)
            {
                scope = merged(scope, read.scope);
            }
        }
        state.write = TrackedUse{ .position = position, .scope = scope };
        state.reads.clear();
    }

    void recordBarrier(QueueHazards& state, uint64_t position, Scope source, Scope destination)
    {
        const auto reach = [&](TrackedUse& use) {
            if (use.position >= position)
            {
                return;
            }
            // A transition is ordered only through a chain, from its own destination, its first reach.
            const bool orders = (!use.transition && sourceCovers(source, use.scope)) || chains(use, position, source);
            // The first barrier of each kind is kept: an earlier one orders whatever a later one of its kind would.
            const Reach added{ .destination = destination, .sourceCovers = orders, .position = position };
            if (std::ranges::none_of(use.reaches, [&](const Reach& reach) { return sameKind(reach, added); }))
            {
                use.reaches.push_back(added);
            }
        };
        if (state.write.has_value())
        {
            reach(*state.write);
        }
        std::ranges::for_each(state.reads, reach);
        // Reads the barrier left alike are judged alike from now on; keep the first of each.
        std::vector<TrackedUse> distinct;
        distinct.reserve(state.reads.size());
        for (TrackedUse& read : state.reads)
        {
            if (std::ranges::none_of(distinct, [&](const TrackedUse& kept) { return alike(kept, read, position); }))
            {
                distinct.push_back(std::move(read));
            }
        }
        state.reads = std::move(distinct);
    }

    Hazard checkTransition(const QueueHazards& state, uint64_t position, Scope source, bool discard, uint64_t fenced)
    {
        // A fence wait or the CPU's observation orders an earlier use as a chain would.
        const auto chained = [&](const TrackedUse& earlier) { return earlier.position <= fenced || chains(earlier, position, source); };
        const auto orderedBefore = [&](const TrackedUse& earlier) { return transitionOrders(source, discard, earlier) || chained(earlier); };
        const auto missed = [](const TrackedUse& use) {
            return Hazard{ .kind = HazardKind::Unordered, .earlier = use.scope, .earlierTransition = use.transition };
        };
        for (const TrackedUse& read : std::views::reverse(state.reads))
        {
            if (!orderedBefore(read))
            {
                return missed(read);
            }
        }
        if (!state.write.has_value())
        {
            return Hazard{};
        }
        // With no use since the last transition, only an execution chain from its destination stage orders the two: the
        // transition's own reach, or a later barrier whose source stage shares a stage with it.
        if (state.write->transition && state.reads.empty())
        {
            return chained(*state.write) ? Hazard{} : missed(*state.write);
        }
        if (orderedBefore(*state.write))
        {
            return Hazard{};
        }
        // Every read is ordered before the transition by now, so one a barrier ordered after the last write chains the two.
        const bool throughRead = std::ranges::any_of(state.reads, [](const TrackedUse& read) { return read.afterWrite; });
        return throughRead ? Hazard{} : missed(*state.write);
    }

    void recordTransition(QueueHazards& state, uint64_t position, Scope destination)
    {
        state.write = TrackedUse{ .position = position,
                                  .scope = destination,
                                  .transition = true,
                                  .reaches = { Reach{ .destination = destination, .sourceCovers = true, .position = position } } };
        state.reads.clear();
    }

    std::optional<QueueHazard> checkOtherQueues(const UnitHazards& unit, QueueType queue, Scope use, const Clock& ordered, bool transition)
    {
        const bool writes = transition || hasWrite(use.access);
        for (size_t index = 0; index < unit.queues.size(); ++index)
        {
            const QueueType other = static_cast<QueueType>(index);
            if (other == queue)
            {
                continue;
            }
            const auto missed = [&](const TrackedUse& earlier) { return (writes || isWrite(earlier)) && !covers(ordered, other, earlier.position); };
            const QueueHazards& state = unit.queues[index];
            if (state.write.has_value() && missed(*state.write))
            {
                return QueueHazard{ .queue = other, .earlier = state.write->scope, .earlierTransition = state.write->transition };
            }
            // Reads conflict only with a write.
            const auto read = std::ranges::find_if(state.reads, missed);
            if (read != state.reads.end())
            {
                return QueueHazard{ .queue = other, .earlier = read->scope };
            }
        }
        return std::nullopt;
    }

    WriteOrder lastWriteOrder(const QueueHazards& state, Scope use, uint64_t fenced)
    {
        if (!state.write.has_value())
        {
            return WriteOrder::None;
        }
        if (state.write->position <= fenced || ordered(*state.write, use))
        {
            return WriteOrder::Ordered;
        }
        const bool reached =
            std::ranges::any_of(state.write->reaches, [&](const Reach& reach) { return destinationCovers(reach.destination, use, isWrite(*state.write)); });
        return reached ? WriteOrder::Unordered : WriteOrder::Missing;
    }

    bool mergeable(const UnitHazards& a, const UnitHazards& b, const std::array<uint64_t, 3>& floor)
    {
        for (size_t queue = 0; queue < a.queues.size(); ++queue)
        {
            if (!mergeable(a.queues[queue], b.queues[queue], floor[queue]))
            {
                return false;
            }
        }
        return true;
    }
} // namespace rojoRHI::core
