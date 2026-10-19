//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Timing.cpp
/// @brief Implements the guard that Metal 4 count pins call before reading counts that include timestamp writes.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Metal4/Metal4Timing.h"

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/ScopeRecord.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>

namespace rojoRHI::test {
    //==================================================================================================================
    void requireTimed(CommandList list)
    {
        const core::CommandListCore& state = core::coreOf(list);
        size_t untimed = 0;
        for (const core::ScopeRecord& record : state.scopeLog)
        {
            if (record.timestamp == core::kNoTimestamp)
            {
                ++untimed;
            }
        }
        if (untimed > 0)
        {
            FAIL(
                std::format(
                    "{} of the {} scopes of list \"{}\" hold no timestamp pair, so its counts lack their timestamp writes: Metal "
                    "refused a counter heap, whose limit holds per process or on the system (run no other GPU test process "
                    "meanwhile), or the frame slot's capacity was passed",
                    untimed,
                    state.scopeLog.size(),
                    state.label
                )
            );
        }
    }

    //==================================================================================================================
    const core::RecordCounts& timedCounts(CommandList list)
    {
        requireTimed(list);
        return core::coreOf(list).counts;
    }
} // namespace rojoRHI::test
