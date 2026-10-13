//----------------------------------------------------------------------------------------------------------------------
/// @file IndexType.h
/// @brief Declares the width of the indices an indexed draw reads.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The width of each index in an index buffer: 16 or 32 bits.
    enum class IndexType : uint8_t
    {
        UInt16, ///< 16-bit indices.
        UInt32, ///< 32-bit indices.
    };
} // namespace rojoRHI
