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
            const ArenaAllocation place = bumpArenaPage(core, list, label.size(), 1, core.arenaSource(), &core);
            std::memcpy(place.memory, label.data(), label.size());
            kept = std::string_view(reinterpret_cast<const char*>(place.memory), label.size());
        }
        list.scopeLog.push_back(ScopeRecord{ kept, depth });
    }
} // namespace rojoRHI::core
