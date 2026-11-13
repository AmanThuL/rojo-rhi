//----------------------------------------------------------------------------------------------------------------------
/// @file Scopes.cpp
/// @brief Implements how a command list records the scopes that `Device::scopeTimings` reports.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Scopes.h"

#include <cstring>

namespace rojoRHI::core {
    //==================================================================================================================
    void recordScope(CommandListCore& list, std::string_view label, std::string_view defaultLabel, uint32_t depth)
    {
        std::string_view kept = defaultLabel;
        if (!label.empty())
        {
            DeviceCore& core = *list.device;
            if (!core.timesScopes())
            {
                // Without scope timings no record is submitted, so only the backend reads the label, inside the entry that
                // recorded it, while the caller's text is alive.
                kept = label;
            }
            else if (!list.scopeCopy.empty() && label == list.scopeCopySource)
            {
                // The list copied the same text earlier in this frame, and that copy lives until the frame slot is rewound.
                kept = list.scopeCopy;
            }
            else
            {
                const ArenaAllocation place = bumpArenaPage(core, list, label.size(), 1, core.arenaSource(), &core);
                std::memcpy(place.memory, label.data(), label.size());
                kept = std::string_view(reinterpret_cast<const char*>(place.memory), label.size());
                list.scopeCopy = kept;
                list.scopeCopySource.assign(label);
            }
        }
        list.scopeLog.push_back(ScopeRecord{ kept, depth });
    }
} // namespace rojoRHI::core
