//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4FrameData.h
/// @brief Declares the Metal 4 per-frame data arena's page policy and its test instrumentation.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/Device.h"

#include <array>
#include <cstdint>

/// Developer tooling, not part of the RHI surface (the Metal4Capture.h precedent): what the Metal 4
/// backend allocates behind CommandList::bindFrameData, so that tests and GPU captures can assert
/// on it. These counters are evidence, not a runtime performance query -- nothing in the renderer
/// reads them, and a caller must not schedule work from them. Deliberately metal-cpp-free.

namespace lmx::rhi::metal4 {

/// Bytes in one normal arena page, and the quantum an oversized page rounds its capacity up to.
///
/// A request bigger than this receives a page of its own size rounded up here rather than a buffer
/// per call, so growth stays proportional to a frame's peak rather than to its draw count.
inline constexpr uint64_t kFrameDataPageBytes = 256 * 1024;

/// Number of frame-in-flight slots, each owning an independent arena.
inline constexpr uint32_t kFrameDataSlotCount = 3;

/// What one frame slot's arena held after the most recent frame that owned it.
struct FrameDataSlotCounters {
    uint32_t pageCount = 0;     ///< Pages the slot owns -- its retained high water, never released.
    uint32_t pagesUsed = 0;     ///< Pages the current or most recent frame allocated out of.
    uint64_t bytesUsed = 0;     ///< Bytes that frame consumed, alignment padding included.
    uint64_t capacityBytes = 0; ///< Total capacity of the pages the slot owns.
};

/// Device-wide frame-data instrumentation, accumulated since device creation.
struct FrameDataCounters {
    uint64_t calls = 0;         ///< bindFrameData calls recorded.
    uint64_t bytes = 0;         ///< Bytes those calls copied, excluding alignment padding.
    uint64_t addressBinds = 0;  ///< Native argument-table address binds those calls performed.
    uint32_t pageCreations = 0; ///< Arena pages created, across every slot and both growth paths.
    std::array<FrameDataSlotCounters, kFrameDataSlotCount> slots{}; ///< Per-slot occupancy.
};

/// Returns the frame-data counters `device` has accumulated. The device must be one createDevice()
/// produced; reading it between frames reports the slot that most recently closed.
FrameDataCounters frameDataCounters(const Device& device);

} // namespace lmx::rhi::metal4
