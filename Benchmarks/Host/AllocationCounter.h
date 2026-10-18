//----------------------------------------------------------------------------------------------------------------------
/// @file AllocationCounter.h
/// @brief Declares the process-wide counter of heap allocations through the malloc zone that malloc dispatches to.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace rojoRHI::bench {
    // Wraps the allocating functions of the zone malloc dispatches to with counting forwarders. Call it first in main,
    // before any other thread exists; later calls return the first call's result.
    std::expected<void, std::string> installAllocationCounter();
    // Allocation calls made by every thread since installation: one per call, a batch counting the blocks it returned.
    // Frees are not counted.
    uint64_t allocationCount();
} // namespace rojoRHI::bench
