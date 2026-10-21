//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Timing.cpp
/// @brief Implements what Metal 4 count pins call: the guard before counts that include timestamp writes, and the encoder labels.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Metal4/Metal4Timing.h"

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/ScopeRecord.h"
#include "Metal4Device.h"

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
                    "{} of the {} scopes of list \"{}\" hold no timestamp pair, so its counts lack their timestamp writes: the "
                    "device was created without DeviceDesc::scopeTimings, Metal refused a counter heap, whose limit holds per "
                    "process or on the system (run no other GPU test process meanwhile), or the frame slot's capacity was passed",
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

    //==================================================================================================================
    uint64_t encoderLabel(CommandList list)
    {
        return CommandListImpl::of(list).state.encoderLabels ? 1 : 0;
    }

    //==================================================================================================================
    uint64_t encoderLabel(Device device)
    {
        return DeviceImpl::of(device).encoderLabels ? 1 : 0;
    }
} // namespace rojoRHI::test
