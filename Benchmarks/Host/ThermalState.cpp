//----------------------------------------------------------------------------------------------------------------------
/// @file ThermalState.cpp
/// @brief Implements the thermal state reader over NSProcessInfo.
//----------------------------------------------------------------------------------------------------------------------
#include "Host/ThermalState.h"

#include <Foundation/Foundation.hpp>

namespace rojoRHI::bench {
    //==================================================================================================================
    std::string_view thermalState()
    {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        const NS::ProcessInfoThermalState state = NS::ProcessInfo::processInfo()->thermalState();
        pool->release();
        switch (state)
        {
        case NS::ProcessInfoThermalStateNominal: return "nominal";
        case NS::ProcessInfoThermalStateFair: return "fair";
        case NS::ProcessInfoThermalStateSerious: return "serious";
        case NS::ProcessInfoThermalStateCritical: return "critical";
        }
        // Foundation defines no other state; a later one reads as the hottest known, so a run in it is discarded.
        return "critical";
    }
} // namespace rojoRHI::bench
