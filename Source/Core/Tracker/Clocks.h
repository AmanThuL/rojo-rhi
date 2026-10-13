//----------------------------------------------------------------------------------------------------------------------
/// @file Clocks.h
/// @brief Declares the vector clocks that order the queues of a device for the validation tracker, and the clocks of a fence.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/QueueType.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace rojoRHI::core {
    /// A vector clock: per queue type, indexed by `QueueType`, the position on that queue through which work is ordered. A use
    /// carries its queue and its position there, so whether a clock covers it is one comparison. Zero covers nothing.
    using Clock = std::array<uint64_t, 3>;

    /// Returns the componentwise maximum of `a` and `b`: what is ordered by either.
    Clock join(const Clock& a, const Clock& b);

    /// Returns the componentwise minimum of `a` and `b`: what is ordered by both.
    Clock meet(const Clock& a, const Clock& b);

    /// Returns true when `clock` covers the use at `position` on `queue`: its component of that queue reaches the position.
    bool covers(const Clock& clock, QueueType queue, uint64_t position);

    /// The clocks of the values submitted for signal on one fence, each the running clock of the queue that signalled it.
    class FenceClocks
    {
      public:
        /// Starts the clocks of a fence created at `initial`, which no submission signals.
        explicit FenceClocks(uint64_t initial);

        /// Notes that `value` was submitted for signal with `clock`. Values only grow on a fence (VU-rojo-queue-0003).
        void signal(uint64_t value, const Clock& clock);

        /// Returns what reaching `value` orders: nothing for a value at most the initial one or above every value submitted for
        /// signal, otherwise the meet of the clocks of every value at least `value`, since any of them may reach it first.
        std::optional<Clock> reached(uint64_t value) const;

        /// Forgets the values whose stored meet `host`, the host clock, covers, all but the last of them: that one stays as a
        /// floor, which a wait on any value from above the initial one up to it reaches. Its clock is at least the true meet
        /// of such a wait, but the host clock covers it, so it orders nothing the host clock does not.
        void prune(const Clock& host);

        /// Returns the number of values kept, the floor included.
        size_t size() const;

      private:
        uint64_t m_initial; ///< The fence's initial value.
        /// Each value submitted for signal, in increasing order, with the meet of its clock and the clocks of every later value,
        /// so the stored clocks rise along the list; the first may be the floor `prune` left.
        std::vector<std::pair<uint64_t, Clock>> m_values;
    };
} // namespace rojoRHI::core
