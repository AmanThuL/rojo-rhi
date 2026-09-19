//----------------------------------------------------------------------------------------------------------------------
/// @file Align.h
/// @brief Provides alignment helpers for RHI memory-layout calculations.
//----------------------------------------------------------------------------------------------------------------------

#pragma once
#include <cstdint>

namespace rojoRHI::base {

/// Rounds `value` up to the next multiple of `alignment`.
///
/// Precondition: `alignment` is a power of two. Deliberately not checked -- every call site
/// passes a compile-time constant (kFrameDataPageBytes and friends), so a violation is a
/// typo caught by reading the constant, not a runtime condition to handle. The mask trick below
/// is what makes the power-of-two requirement load-bearing: it clears the low bits, which only
/// equals "round up to a multiple" when exactly those bits span the alignment.
constexpr uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace rojoRHI::base
