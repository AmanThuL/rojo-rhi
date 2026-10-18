//----------------------------------------------------------------------------------------------------------------------
/// @file FeedbackCollector.h
/// @brief Declares the receiver of a frame's commit feedback, which reports the frame's GPU span once every commit has.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace rojoRHI::bench {
    // One commit's GPU start and end, in Metal's seconds.
    struct CommitSpan
    {
        double gpuStartSeconds = 0.0;
        double gpuEndSeconds = 0.0;
        bool failed = false;
    };

    // Collects the feedback of one frame's commits at a time. The frame thread calls begin before its first commit and
    // wait after its last; report may run on any thread, such as Metal's completion thread. Its slots are allocated
    // once, at construction, so neither report nor wait allocates. Each begin opens a new frame, numbered from 1, and
    // wait closes it; a report tagged with another frame, or arriving while no frame is open, fails the next wait.
    class FeedbackCollector
    {
      public:
        static constexpr uint32_t kDefaultCapacity = 1024;
        static constexpr std::chrono::seconds kDefaultTimeout{ 60 };

        explicit FeedbackCollector(uint32_t capacity = kDefaultCapacity);

        void begin(uint32_t expectedCommits);
        // A report for the frame open now.
        void report(double gpuStartSeconds, double gpuEndSeconds, bool failed);
        // A report for `frame`, the value frame() returned when the commit was made.
        void report(uint64_t frame, double gpuStartSeconds, double gpuEndSeconds, bool failed);
        // The frame begun last; 0 before the first begin.
        uint64_t frame() const;
        // The commits every begin so far has announced, in total.
        uint64_t expectedSoFar() const;
        // The frame's span from the earliest start to the latest end, in nanoseconds, once every expected commit has
        // reported; an error names a report outside its frame, a failed commit, more reports than expected, or the reports
        // missing at the timeout. Either way the frame is closed.
        std::expected<double, std::string> wait(std::chrono::nanoseconds timeout = kDefaultTimeout);
        // The reports of the frame begun last, in arrival order; read after wait.
        std::span<const CommitSpan> commits() const;

      private:
        mutable std::mutex m_mutex;
        std::condition_variable m_reported;
        std::vector<CommitSpan> m_slots;
        uint32_t m_expected = 0;
        uint32_t m_received = 0;
        uint64_t m_frame = 0;
        uint64_t m_expectedSoFar = 0;
        uint64_t m_stray = 0; ///< Reports outside their frame since construction; never reset.
        bool m_open = false;
    };
} // namespace rojoRHI::bench
