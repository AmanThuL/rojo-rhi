//----------------------------------------------------------------------------------------------------------------------
/// @file Label.h
/// @brief Declares the label check and default shared by every object a device creates.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <string>
#include <string_view>

namespace rojoRHI::core {
    /// Checks a caller's label (VU-rojo-core-0006 valid UTF-8, VU-rojo-core-0007 no NUL) and returns the string the object
    /// carries: the label verbatim, or `defaultLabel` when it is empty.
    std::string resolveLabel(std::string_view label, std::string_view defaultLabel);

    /// Checks a caller's label as `resolveLabel` does (VU-rojo-core-0006 valid UTF-8, VU-rojo-core-0007 no NUL), for a caller
    /// that copies the label into a string of its own.
    void checkLabel(std::string_view label);

    /// Returns true when `text` is well-formed UTF-8: no overlong form, surrogate or code point above U+10FFFF.
    bool isValidUtf8(std::string_view text);
} // namespace rojoRHI::core
