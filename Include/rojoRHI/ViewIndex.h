//----------------------------------------------------------------------------------------------------------------------
/// @file ViewIndex.h
/// @brief Declares the 32-bit index of a texture view in the global view heap.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// A 32-bit index into the global view heap, stored in shader data with `std::to_underlying`.
    enum class ViewIndex : uint32_t
    {
    };
} // namespace rojoRHI
