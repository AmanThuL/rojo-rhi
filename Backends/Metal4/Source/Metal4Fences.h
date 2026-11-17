//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Fences.h
/// @brief Declares the retire hook of Metal 4 fences and the release of destroyed fences' events once no queue uses them.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/Records.h"

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::metal4 {
    // Runs under the fence pool's lock.
    void retireFence(void* context, core::FenceRecord& record);

    // Reads a queue's progress event, one counted native host call, only when a kept event waits for that queue.
    void releasePassedFences(DeviceImpl& impl);

    // At teardown, once no queue holds work; the caller holds an autorelease pool.
    void releaseRetiringFences(DeviceImpl& impl);
} // namespace rojoRHI::metal4
