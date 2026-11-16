//----------------------------------------------------------------------------------------------------------------------
/// @file ScopeLog.cpp
/// @brief Implements the per-slot record of submitted scopes and the report of the frame that retired.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/ScopeLog.h"

#include "Base/Assert.h"

#include <cstddef>

namespace rojoRHI::core {
    //==================================================================================================================
    ScopeLog::ScopeLog(uint32_t slots) : m_slots(slots)
    {
    }

    //==================================================================================================================
    void ScopeLog::submit(uint32_t slot, QueueType queue, std::span<const ScopeRecord> scopes)
    {
        if (scopes.empty())
        {
            return;
        }
        std::lock_guard lock(m_mutex);
        ROJORHI_INTERNAL_ASSERT(slot < m_slots.size(), "scopes were submitted for a slot the log does not have");
        std::vector<ScopeRecord>& record = m_slots[slot][static_cast<size_t>(queue)];
        record.insert(record.end(), scopes.begin(), scopes.end());
    }

    //==================================================================================================================
    void ScopeLog::clearReport()
    {
        m_report.clear();
        m_labels.clear();
    }

    //==================================================================================================================
    void ScopeLog::retire(uint32_t slot, uint64_t frame, ScopeTimesFn scopeTimes, void* context)
    {
        std::lock_guard lock(m_mutex);
        ROJORHI_INTERNAL_ASSERT(slot < m_slots.size(), "a slot the log does not have was retired");
        std::array<std::vector<ScopeRecord>, 3>& record = m_slots[slot];
        // The labels are copied first, so the views are taken once the storage no longer moves.
        for (const std::vector<ScopeRecord>& queue : record)
        {
            for (const ScopeRecord& scope : queue)
            {
                m_labels.emplace_back(scope.label);
            }
        }
        const size_t first = m_report.size();
        size_t next = 0;
        for (size_t queue = 0; queue < record.size(); ++queue)
        {
            for (const ScopeRecord& scope : record[queue])
            {
                m_report.push_back(ScopeTiming{ .label = m_labels[next++], .frame = frame, .queue = static_cast<QueueType>(queue), .depth = scope.depth });
            }
        }
        // The report no longer grows, so each queue's entries are handed over as a span; the records' labels still view the
        // slot's arena, which the caller rewinds after this returns.
        size_t at = first;
        for (std::vector<ScopeRecord>& queue : record)
        {
            if (scopeTimes != nullptr && !queue.empty())
            {
                scopeTimes(context, slot, queue, std::span(m_report).subspan(at, queue.size()));
            }
            at += queue.size();
            queue.clear();
        }
    }

    //==================================================================================================================
    std::span<const ScopeTiming> ScopeLog::report() const
    {
        return m_report;
    }
} // namespace rojoRHI::core
