//----------------------------------------------------------------------------------------------------------------------
/// @file SamplerIndex.h
/// @brief Declares the 32-bit index of a sampler in the global sampler heap.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// A 32-bit index into the global sampler heap, stored in shader data with `std::to_underlying`.
    enum class SamplerIndex : uint32_t
    {
    };
} // namespace rojoRHI
