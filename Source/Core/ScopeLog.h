//----------------------------------------------------------------------------------------------------------------------
/// @file ScopeLog.h
/// @brief Declares the scopes a device collects per frame slot and the ones it reports for the frame that retired.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/ScopeRecord.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace rojoRHI::core {
    /// The scopes of the frames in flight, one record per queue type for each frame slot, and the report for the frame that
    /// retired. A list's scopes join its slot's record at submission, which takes the log's lock; recording takes none. When a
    /// slot is reused its scopes move into the report, with each label copied into storage the log owns, because the arena
    /// pages the labels lived in are claimable again from that frame on.
    class ScopeLog
    {
      public:
        /// Creates a log with `slots` empty frame slots.
        explicit ScopeLog(uint32_t slots);

        /// Appends `scopes`, recorded by a list for `queue`, to the record of `slot`. Thread-safe.
        void submit(uint32_t slot, QueueType queue, std::span<const ScopeRecord> scopes);

        /// Empties the report; every span `report` returned is invalid afterwards. `DeviceCore::beginFrame` calls it first.
        void clearReport();

        /// Moves the record of `slot`, the scopes of frame `frame`, into the report: queues in `QueueType` order, each in
        /// submission order, with a duration of zero, and empties the record. Labels are copied. Then, unless `scopeTimes` is
        /// null, calls it with `context` once per queue that has scopes, in the same order, before the record is emptied; a
        /// null `scopeTimes` leaves every duration zero.
        void retire(uint32_t slot, uint64_t frame, ScopeTimesFn scopeTimes, void* context);

        /// Returns the report of the latest retired frame; empty until a frame has retired.
        std::span<const ScopeTiming> report() const;

      private:
        std::mutex m_mutex;
        std::vector<std::array<std::vector<ScopeRecord>, 3>> m_slots;
        std::vector<std::string> m_labels;
        std::vector<ScopeTiming> m_report;
    };
} // namespace rojoRHI::core
