//----------------------------------------------------------------------------------------------------------------------
/// @file RojoGpuTiming.h
/// @brief Declares the RojoRHI arm's commit timing: the backend's commit reports, mapped to frames by queue and serial.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/FeedbackCollector.h"

#include <rojoRHI/RHI.h>

#include <array>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <vector>

namespace rojoRHI::bench {
    // Receives the GPU time of every commit of the device's three queues and passes it to the collector, tagged with
    // the frame it belongs to. Each queue numbers its commits from 1 and frames run one after another, so the serials
    // of a frame's commits on a queue follow those of the frame before it without a gap. A report whose serial a
    // closed frame already took belongs to no open frame and fails the next wait; closeFrame, after a successful wait,
    // fails unless the frame's serials on each queue continue exactly where the last frame stopped. Neither path
    // allocates: the accepted serials live in slots reserved at construction.
    class RojoGpuTiming
    {
      public:
        explicit RojoGpuTiming(FeedbackCollector& collector);
        RojoGpuTiming(const RojoGpuTiming&) = delete;
        RojoGpuTiming& operator=(const RojoGpuTiming&) = delete;

        // Reports every later commit of the device's queues here. False on a backend with no GPU time, Null, where the
        // RojoRHI arm reports none.
        bool attach(Device device);
        void detach(Device device);
        // Makes the backend report every commit of the device's queues made so far, on this thread, once each has
        // completed: D3D12 reports only when drained; Metal 4 reports from its completion thread, so there it does nothing.
        void drain(Device device);

        // One commit's time, from the backend's completion thread, the thread that drains, or a check.
        void report(QueueType queue, uint64_t serial, double gpuStartSeconds, double gpuEndSeconds, bool failed);
        // After the frame's wait succeeded: checks and advances each queue's serials.
        std::expected<void, std::string> closeFrame();

      private:
        struct Accepted
        {
            uint32_t queue;
            uint64_t serial;
        };

        static constexpr uint32_t kQueues = 3;

        FeedbackCollector& m_collector;
        std::mutex m_mutex;
        std::array<uint64_t, kQueues> m_closed{}; ///< The last serial of each queue that a closed frame took.
        std::vector<Accepted> m_accepted;
    };
} // namespace rojoRHI::bench
