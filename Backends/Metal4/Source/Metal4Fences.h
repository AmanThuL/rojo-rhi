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
    /// Releases the shared event of a retiring fence record. `context` is the `DeviceImpl`; a null payload is skipped. It runs
    /// under the fence pool's lock.
    void retireFence(void* context, core::FenceRecord& record);

    /// Releases the events of destroyed fences that every queue's progress event has passed, reading a queue's progress
    /// event, one counted native host call, only when a kept event waits for that queue. It runs when a frame slot is reused
    /// and after a destroyed fence's event is kept.
    void releasePassedFences(DeviceImpl& impl);

    /// Releases every kept event of a destroyed fence, at teardown, once no queue holds work; the caller holds a pool.
    void releaseRetiringFences(DeviceImpl& impl);
} // namespace rojoRHI::metal4
