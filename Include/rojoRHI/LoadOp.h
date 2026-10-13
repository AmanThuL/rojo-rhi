//----------------------------------------------------------------------------------------------------------------------
/// @file LoadOp.h
/// @brief Declares what an attachment holds when a render pass begins.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// What an attachment holds when a render pass begins.
    enum class LoadOp : uint8_t
    {
        Load,     ///< The attachment's earlier contents.
        Clear,    ///< The attachment's clear value.
        DontCare, ///< Undefined contents the pass must overwrite.
    };
} // namespace rojoRHI
