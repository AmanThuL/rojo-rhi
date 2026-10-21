//----------------------------------------------------------------------------------------------------------------------
/// @file AllocationCounter.h
/// @brief Declares the process-wide counter of heap allocations: malloc zone 0 on macOS, operator new on Windows.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace rojoRHI::bench {
    // On macOS, wraps the allocating functions of the zone malloc dispatches to with counting forwarders; on Windows, where
    // the executable replaces the global operator new and delete in every form, checks that each form counts once. Call it
    // first in main, before any other thread exists; later calls return the first call's result.
    std::expected<void, std::string> installAllocationCounter();
    // Allocation calls made by every thread since installation: one per call, a batch counting the blocks it returned.
    // Frees are not counted. Windows counts operator new calls only, so a direct malloc is not seen there.
    uint64_t allocationCount();
} // namespace rojoRHI::bench
