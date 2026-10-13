//----------------------------------------------------------------------------------------------------------------------
/// @file CompareOp.h
/// @brief Declares the comparison function shared by depth and stencil tests and by comparison samplers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The one comparison function of the interface: a test passes when `incoming op stored` holds, or always (`Always`) or
    /// never (`Never`).
    enum class CompareOp : uint8_t
    {
        Never,          ///< The test never passes.
        Less,           ///< Passes when incoming is less than stored.
        Equal,          ///< Passes when incoming equals stored.
        LessOrEqual,    ///< Passes when incoming is less than or equal to stored.
        Greater,        ///< Passes when incoming is greater than stored.
        NotEqual,       ///< Passes when incoming differs from stored.
        GreaterOrEqual, ///< Passes when incoming is greater than or equal to stored.
        Always,         ///< The test always passes.
    };
} // namespace rojoRHI
