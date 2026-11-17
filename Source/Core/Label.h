//----------------------------------------------------------------------------------------------------------------------
/// @file Label.h
/// @brief Declares the label check and default shared by every object a device creates.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <string>
#include <string_view>

namespace rojoRHI::core {
    // The label must be valid UTF-8 with no NUL; an empty one becomes `defaultLabel`.
    std::string resolveLabel(std::string_view label, std::string_view defaultLabel);

    void checkLabel(std::string_view label);

    // Rejects overlong forms, surrogates and code points above U+10FFFF.
    bool isValidUtf8(std::string_view text);
} // namespace rojoRHI::core
