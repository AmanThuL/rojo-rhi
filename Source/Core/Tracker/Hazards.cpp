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
        /// Every access bit that writes.
        constexpr Access kWrites = Access::ShaderWrite | Access::ColorAttachmentWrite | Access::DepthStencilWrite | Access::CopyWrite | Access::Resolve;

        //==============================================================================================================
        /// Returns true when `earlier` is a write, a layout transition included.
        bool isWrite(const TrackedUse& earlier)
        {
            return earlier.transition || hasWrite(earlier.scope.access);
        }

        //==============================================================================================================
        /// Returns true when a barrier since `earlier` orders it before `later`: one reach that orders `earlier`, its source
        /// covering it or chaining from a barrier that does, and whose destination covers `later`.
        bool ordered(const TrackedUse& earlier, Scope later)
        {
            const bool writes = isWrite(earlier);
            return std::ranges::any_of(earlier.reaches, [&](const Reach& reach) {
                return reach.sourceCovers && destinationCovers(reach.destination, later, writes);
            });
        }

        //==============================================================================================================
        /// Returns the hazard of the pair `earlier`, `later` when it is not ordered: no barrier between, or barriers none of
        /// which orders it.
        Hazard unordered(const TrackedUse& earlier)
        {
            return Hazard{ .kind = earlier.reaches.empty() ? HazardKind::NoBarrier : HazardKind::Unordered,
                           .earlier = earlier.scope,
                           .earlierTransition = earlier.transition };
        }

        //==============================================================================================================
        /// Returns true when `a` and `b` are reaches of the same kind: the same destination and source coverage.
        bool sameKind(const Reach& a, const Reach& b)
        {
            return a.destination == b.destination && a.sourceCovers == b.sourceCovers;
        }

        //==============================================================================================================
        /// Returns true when `a` and `b` hold the same reaches in any order, each list holding a kind once. A reach's position
        /// matters only when it is at least `fresh`, the position of a barrier call still being replayed, whose records never
        /// order a transition of the same call; positions below it compare alike.
        bool sameReaches(const std::vector<Reach>& a, const std::vector<Reach>& b, uint64_t fresh)
        {
            return a.size() == b.size() && std::ranges::all_of(a, [&](const Reach& reach) {
                       return std::ranges::any_of(b, [&](const Reach& other) {
                           return sameKind(reach, other) && (reach.position >= fresh) == (other.position >= fresh);
                       });
                   });
        }

        //==============================================================================================================
        /// Returns true when `a` and `b` are judged alike by every later use, transition and barrier: all but their positions
        /// are equal, and their reaches are alike as `sameReaches` reads them from `fresh`. Positions only tell a later use of
        /// the same command apart, and an earlier use alike to it with no barrier since conflicts with that later use as the
        /// dropped one would.
        bool alike(const TrackedUse& a, const TrackedUse& b, uint64_t fresh)
        {
            return a.scope == b.scope && a.transition == b.transition && a.afterWrite == b.afterWrite && sameReaches(a.reaches, b.reaches, fresh);
        }

        //==============================================================================================================
        /// Returns true when the states `a` and `b` of one queue are alike and every use in them is below `floor`.
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

        //==============================================================================================================
        /// Returns the union of `a` and `b`.
        Scope merged(Scope a, Scope b)
        {
            return Scope{ .stage = a.stage | b.stage, .access = a.access | b.access };
        }

        //==============================================================================================================
        /// Returns true when a barrier from `source` recorded at `position` extends the execution chain of an earlier barrier
        /// over `use`: one that orders the use, its source covering it or chaining in turn, and whose destination stage shares a
        /// stage with `source`. A record of the same barrier call, at `position`, never chains.
        bool chains(const TrackedUse& use, uint64_t position, Scope source)
        {
            return std::ranges::any_of(use.reaches, [&](const Reach& reach) {
                return reach.position != position && reach.sourceCovers && sharesStage(reach.destination.stage, source.stage);
            });
        }

        //==============================================================================================================
        /// Returns true when a layout transition's own `source` orders `earlier` before the transition: it covers a use, or,
        /// for an earlier transition, shares a stage with its destination.
        bool transitionOrders(Scope source, bool discard, const TrackedUse& earlier)
        {
            return earlier.transition ? sharesStage(source.stage, earlier.scope.stage) : sourceCovers(source, earlier.scope, discard);
        }
    } // namespace

    //==================================================================================================================
    bool sourceCovers(Scope source, Scope use, bool discard)
    {
        // A source with no stage waits for nothing, so it covers no use.
        const Access writes = use.access & kWrites;
        return source.stage != Stage::None && (source.stage & use.stage) == use.stage && (discard || (source.access & writes) == writes);
    }

    //==================================================================================================================
    bool destinationCovers(Scope destination, Scope use, bool earlierWrites)
    {
        return (destination.stage & use.stage) == use.stage && (!earlierWrites || (destination.access & use.access) == use.access);
    }

    //==================================================================================================================
    bool sharesStage(Stage a, Stage b)
    {
        return (a & b) != Stage::None;
    }

    //==================================================================================================================
    bool hasWrite(Access access)
    {
        return (access & kWrites) != Access::None;
    }

    //==================================================================================================================
    Hazard checkUse(const QueueHazards& state, uint64_t position, Scope use)
    {
        const bool otherWrite = state.write.has_value() && state.write->position != position;
        if (!hasWrite(use.access))
        {
            return otherWrite && !ordered(*state.write, use) ? unordered(*state.write) : Hazard{};
        }
        // A write after reads: each read, latest first, then the write before them, which a read between may order.
        for (const TrackedUse& read : std::views::reverse(state.reads))
        {
            if (read.position != position && !ordered(read, use))
            {
                return unordered(read);
            }
        }
        if (!otherWrite || ordered(*state.write, use))
        {
            return Hazard{};
        }
        const bool chained =
            std::ranges::any_of(state.reads, [&](const TrackedUse& read) { return read.afterWrite && read.position != position && ordered(read, use); });
        return chained ? Hazard{} : unordered(*state.write);
    }

    //==================================================================================================================
    void recordUse(QueueHazards& state, uint64_t position, Scope use)
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
            const TrackedUse read{ .position = position, .scope = use, .afterWrite = state.write.has_value() && ordered(*state.write, use) };
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

    //==================================================================================================================
    void recordBarrier(QueueHazards& state, uint64_t position, Scope source, Scope destination)
    {
        const auto reach = [&](TrackedUse& use) {
            if (use.position >= position)
            {
                return;
            }
            // A barrier orders the use when its source covers it, or through an execution chain: its source stage shares a stage
            // with the destination stage of an earlier barrier that orders the use. A transition is ordered only through a chain,
            // from its own destination, its first reach.
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

    //==================================================================================================================
    Hazard checkTransition(const QueueHazards& state, uint64_t position, Scope source, bool discard)
    {
        // The transition waits only through an execution chain: its own source covers an earlier use, or shares a stage with
        // the destination of an earlier barrier that orders it. A source with no stage waits for nothing, and the records of
        // the transition's own barrier call, at its position, never order it.
        const auto chained = [&](const TrackedUse& earlier) { return chains(earlier, position, source); };
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

    //==================================================================================================================
    void recordTransition(QueueHazards& state, uint64_t position, Scope destination)
    {
        state.write = TrackedUse{ .position = position,
                                  .scope = destination,
                                  .transition = true,
                                  .reaches = { Reach{ .destination = destination, .sourceCovers = true, .position = position } } };
        state.reads.clear();
    }

    //==================================================================================================================
    WriteOrder lastWriteOrder(const QueueHazards& state, Scope use)
    {
        if (!state.write.has_value())
        {
            return WriteOrder::None;
        }
        if (ordered(*state.write, use))
        {
            return WriteOrder::Ordered;
        }
        const bool reached =
            std::ranges::any_of(state.write->reaches, [&](const Reach& reach) { return destinationCovers(reach.destination, use, isWrite(*state.write)); });
        return reached ? WriteOrder::Unordered : WriteOrder::Missing;
    }

    //==================================================================================================================
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
