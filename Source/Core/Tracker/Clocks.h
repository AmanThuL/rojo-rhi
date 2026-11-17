//----------------------------------------------------------------------------------------------------------------------
/// @file Clocks.h
/// @brief Declares the vector clocks that order the queues of a device for the validation tracker, and the clocks of a fence.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace rojoRHI::core {
    // Per queue type, indexed by `QueueType`, the position on that queue through which work is ordered. Zero covers nothing.
    using Clock = std::array<uint64_t, 3>;

    Clock join(const Clock& a, const Clock& b);

    Clock meet(const Clock& a, const Clock& b);

    bool covers(const Clock& clock, QueueType queue, uint64_t position);

    // Each value submitted for signal carries the running clock of the queue that signalled it.
    class FenceClocks
    {
      public:
        explicit FenceClocks(uint64_t initial);

        // Values only grow on a fence (VU-rojo-queue-0003).
        void signal(uint64_t value, const Clock& clock);

        // The meet of the clocks of every value at least `value`, since any of them may reach it first; nothing for a value at
        // most the initial one or above every value submitted.
        std::optional<Clock> reached(uint64_t value) const;

        // Forgets the values the host clock covers but the last, which stays as the floor any wait up to it reaches. Its clock
        // may exceed such a wait's true meet, but the host clock covers it, so it orders nothing the host clock does not.
        void prune(const Clock& host);

        size_t size() const;

      private:
        uint64_t m_initial;
        // In increasing value order, each with the meet of its clock and every later value's, so the stored clocks rise along
        // the list; the first may be the floor `prune` left.
        std::vector<std::pair<uint64_t, Clock>> m_values;
    };
} // namespace rojoRHI::core
