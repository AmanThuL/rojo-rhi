//----------------------------------------------------------------------------------------------------------------------
/// @file Clocks.cpp
/// @brief Implements the vector clocks that order the queues of a device, and the clocks of a fence.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Tracker/Clocks.h"

#include "Base/Assert.h"

#include <algorithm>
#include <cstddef>
#include <ranges>

namespace rojoRHI::core {
    //==================================================================================================================
    Clock join(const Clock& a, const Clock& b)
    {
        Clock joined{};
        for (size_t queue = 0; queue < joined.size(); ++queue)
        {
            joined[queue] = std::max(a[queue], b[queue]);
        }
        return joined;
    }

    //==================================================================================================================
    Clock meet(const Clock& a, const Clock& b)
    {
        Clock met{};
        for (size_t queue = 0; queue < met.size(); ++queue)
        {
            met[queue] = std::min(a[queue], b[queue]);
        }
        return met;
    }

    //==================================================================================================================
    bool covers(const Clock& clock, QueueType queue, uint64_t position)
    {
        return position != 0 && position <= clock[static_cast<size_t>(queue)];
    }

    //==================================================================================================================
    FenceClocks::FenceClocks(uint64_t initial) : m_initial(initial)
    {
    }

    //==================================================================================================================
    void FenceClocks::signal(uint64_t value, const Clock& clock)
    {
        ROJORHI_INTERNAL_ASSERT(value > m_initial && (m_values.empty() || value > m_values.back().first), "a fence value was signalled out of order");
        // Each earlier value may be reached by this one first, so its meet takes this clock in; once an earlier meet is already
        // below it, every meet before that one is too.
        for (auto& [earlier, met] : std::views::reverse(m_values))
        {
            const Clock lowered = meet(met, clock);
            if (lowered == met)
            {
                break;
            }
            met = lowered;
        }
        m_values.emplace_back(value, clock);
    }

    //==================================================================================================================
    void FenceClocks::prune(const Clock& host)
    {
        // The stored meets rise along the list, since each is at most the next, so the values the host clock covers are a
        // prefix; the last of them stays as the floor every earlier value now reaches.
        size_t floor = 0;
        while (floor + 1 < m_values.size() && join(m_values[floor + 1].second, host) == host)
        {
            ++floor;
        }
        if (floor != 0)
        {
            m_values.erase(m_values.begin(), m_values.begin() + static_cast<std::ptrdiff_t>(floor));
        }
    }

    //==================================================================================================================
    size_t FenceClocks::size() const
    {
        return m_values.size();
    }

    //==================================================================================================================
    std::optional<Clock> FenceClocks::reached(uint64_t value) const
    {
        if (value <= m_initial)
        {
            return std::nullopt;
        }
        const auto found = std::ranges::lower_bound(m_values, value, {}, &std::pair<uint64_t, Clock>::first);
        if (found == m_values.end())
        {
            return std::nullopt;
        }
        return found->second;
    }
} // namespace rojoRHI::core
