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
    // A list's scopes join its slot's record at submission, under the log's lock; recording takes none. Retiring a slot copies
    // its labels into storage the log owns, because the arena pages they lived in are claimable again from that frame on.
    class ScopeLog
    {
      public:
        explicit ScopeLog(uint32_t slots);

        // Thread-safe.
        void submit(uint32_t slot, QueueType queue, std::span<const ScopeRecord> scopes);

        // Invalidates every span `report` returned.
        void clearReport();

        // Queues in `QueueType` order, each in submission order; `scopeTimes`, unless null, fills each queue's durations.
        void retire(uint32_t slot, uint64_t frame, ScopeTimesFn scopeTimes, void* context);

        std::span<const ScopeTiming> report() const;

      private:
        std::mutex m_mutex;
        std::vector<std::array<std::vector<ScopeRecord>, 3>> m_slots;
        std::vector<std::string> m_labels;
        std::vector<ScopeTiming> m_report;
    };
} // namespace rojoRHI::core
