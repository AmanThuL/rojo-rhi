//----------------------------------------------------------------------------------------------------------------------
/// @file FeedbackCollector.cpp
/// @brief Implements the commit feedback collector over preallocated slots, a mutex and a condition variable.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/FeedbackCollector.h"

#include <algorithm>
#include <format>

namespace rojoRHI::bench {
    //==================================================================================================================
    FeedbackCollector::FeedbackCollector(uint32_t capacity) : m_slots(capacity)
    {
    }

    //==================================================================================================================
    void FeedbackCollector::begin(uint32_t expectedCommits)
    {
        const std::lock_guard lock(m_mutex);
        ++m_frame;
        m_expected = expectedCommits;
        m_received = 0;
        m_expectedSoFar += expectedCommits;
        m_open = true;
    }

    //==================================================================================================================
    void FeedbackCollector::report(double gpuStartSeconds, double gpuEndSeconds, bool failed)
    {
        report(frame(), gpuStartSeconds, gpuEndSeconds, failed);
    }

    //==================================================================================================================
    // Notifies while holding the lock: once the frame thread can take the lock after this report, the reporting thread
    // touches the collector no more, so the frame thread may destroy it as soon as wait returns.
    void FeedbackCollector::report(uint64_t frame, double gpuStartSeconds, double gpuEndSeconds, bool failed)
    {
        const std::lock_guard lock(m_mutex);
        if (!m_open || frame != m_frame)
        {
            ++m_stray;
        }
        else
        {
            // A report past the slots is still counted, so wait sees more reports than expected and fails.
            if (m_received < m_slots.size())
            {
                m_slots[m_received] = CommitSpan{ gpuStartSeconds, gpuEndSeconds, failed };
            }
            ++m_received;
        }
        m_reported.notify_all();
    }

    //==================================================================================================================
    uint64_t FeedbackCollector::frame() const
    {
        const std::lock_guard lock(m_mutex);
        return m_frame;
    }

    //==================================================================================================================
    uint64_t FeedbackCollector::expectedSoFar() const
    {
        const std::lock_guard lock(m_mutex);
        return m_expectedSoFar;
    }

    //==================================================================================================================
    std::expected<double, std::string> FeedbackCollector::wait(std::chrono::nanoseconds timeout)
    {
        std::unique_lock lock(m_mutex);
        const bool complete = m_reported.wait_for(lock, timeout, [this] { return m_stray > 0 || m_received >= m_expected; });
        m_open = false;
        if (m_stray > 0)
        {
            return std::unexpected(std::format("{} commit reports arrived outside the frame that made the commit", m_stray));
        }
        if (!complete)
        {
            return std::unexpected(std::format("{} of {} commits reported their feedback before the timeout", m_received, m_expected));
        }
        if (m_received > m_expected || m_received > m_slots.size())
        {
            return std::unexpected(std::format("{} commits reported, {} expected", m_received, m_expected));
        }
        double start = 0.0;
        double end = 0.0;
        for (uint32_t index = 0; index < m_received; ++index)
        {
            const CommitSpan& span = m_slots[index];
            if (span.failed)
            {
                return std::unexpected(std::format("commit {} of {} failed on the GPU", index + 1, m_expected));
            }
            start = index == 0 ? span.gpuStartSeconds : std::min(start, span.gpuStartSeconds);
            end = index == 0 ? span.gpuEndSeconds : std::max(end, span.gpuEndSeconds);
        }
        return (end - start) * 1.0e9;
    }

    //==================================================================================================================
    std::span<const CommitSpan> FeedbackCollector::commits() const
    {
        return std::span<const CommitSpan>(m_slots).first(std::min<size_t>(m_received, m_slots.size()));
    }
} // namespace rojoRHI::bench
