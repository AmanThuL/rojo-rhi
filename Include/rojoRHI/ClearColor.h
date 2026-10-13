//----------------------------------------------------------------------------------------------------------------------
/// @file ClearColor.h
/// @brief Declares the clear value of a color attachment.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

namespace rojoRHI {
    /// The value a color attachment is cleared to; an integer format receives the components converted to integers.
    struct ClearColor
    {
        float r = 0.0f; ///< Red.
        float g = 0.0f; ///< Green.
        float b = 0.0f; ///< Blue.
        float a = 0.0f; ///< Alpha.
    };
} // namespace rojoRHI
