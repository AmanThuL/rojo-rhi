//----------------------------------------------------------------------------------------------------------------------
/// @file StoreOp.h
/// @brief Declares whether an attachment's contents are kept when a render pass ends.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// Whether an attachment's rendered contents are kept when the render pass ends. With `DontCare` the contents are undefined
    /// afterwards; a multisample resolve still happens.
    enum class StoreOp : uint8_t
    {
        Store,    ///< Keep the contents.
        DontCare, ///< The contents are undefined afterwards.
    };
} // namespace rojoRHI
