//----------------------------------------------------------------------------------------------------------------------
/// @file RojoGpuTiming.cpp
/// @brief Implements the RojoRHI arm's commit timing over the Metal 4 backend's commit timing seam.
//----------------------------------------------------------------------------------------------------------------------
#include "Rojo/RojoGpuTiming.h"

#if defined(ROJORHI_BENCH_BACKEND_METAL4)
#include "CommitTiming.h"
#endif

#include <algorithm>
#include <format>

namespace rojoRHI::bench {
    namespace {
        constexpr QueueType kQueueTypes[] = { QueueType::Graphics, QueueType::Compute, QueueType::Copy };

        //==============================================================================================================
        const char* queueName(uint32_t queue)
        {
            return queue == 0 ? "graphics" : queue == 1 ? "compute" : "copy";
        }
    } // namespace

    //==================================================================================================================
    RojoGpuTiming::RojoGpuTiming(FeedbackCollector& collector) : m_collector(collector)
    {
        m_accepted.reserve(FeedbackCollector::kDefaultCapacity);
    }

    //==================================================================================================================
    bool RojoGpuTiming::attach(Device device)
    {
#if defined(ROJORHI_BENCH_BACKEND_METAL4)
        // Runs on Metal's completion thread.
        constexpr metal4::CommitTimingCallback callback = [](void* user, const metal4::CommitTime& time) {
            static_cast<RojoGpuTiming*>(user)->report(time.queue, time.serial, time.gpuStartSeconds, time.gpuEndSeconds, time.failed);
        };
        for (const QueueType type : kQueueTypes)
        {
            metal4::setCommitTiming(device.queue(type), callback, this);
        }
        return true;
#else
        (void)device;
        (void)kQueueTypes;
        return false;
#endif
    }

    //==================================================================================================================
    void RojoGpuTiming::detach(Device device)
    {
#if defined(ROJORHI_BENCH_BACKEND_METAL4)
        for (const QueueType type : kQueueTypes)
        {
            metal4::setCommitTiming(device.queue(type), nullptr, nullptr);
        }
#else
        (void)device;
#endif
    }

    //==================================================================================================================
    // Frame 0 is never open, so a report the open frame cannot own fails the collector's next wait.
    void RojoGpuTiming::report(QueueType queue, uint64_t serial, double gpuStartSeconds, double gpuEndSeconds, bool failed)
    {
        const std::lock_guard lock(m_mutex);
        const auto index = static_cast<uint32_t>(queue);
        const bool open = index < kQueues && serial > m_closed[index] && m_accepted.size() < m_accepted.capacity();
        if (open)
        {
            m_accepted.push_back(Accepted{ index, serial });
        }
        m_collector.report(open ? m_collector.frame() : 0, gpuStartSeconds, gpuEndSeconds, failed);
    }

    //==================================================================================================================
    std::expected<void, std::string> RojoGpuTiming::closeFrame()
    {
        const std::lock_guard lock(m_mutex);
        std::ranges::sort(m_accepted, [](const Accepted& a, const Accepted& b) { return a.queue != b.queue ? a.queue < b.queue : a.serial < b.serial; });
        std::array<uint64_t, kQueues> next = m_closed;
        for (const Accepted& accepted : m_accepted)
        {
            if (accepted.serial != next[accepted.queue] + 1)
            {
                m_accepted.clear();
                return std::unexpected(
                    std::format(
                        "{} queue commit serial {} reported where serial {} was due", queueName(accepted.queue), accepted.serial, next[accepted.queue] + 1
                    )
                );
            }
            next[accepted.queue] = accepted.serial;
        }
        m_closed = next;
        m_accepted.clear();
        return {};
    }
} // namespace rojoRHI::bench
