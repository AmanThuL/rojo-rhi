//----------------------------------------------------------------------------------------------------------------------
/// @file Json.cpp
/// @brief Implements byte-preserving JSON string-content escaping.
//----------------------------------------------------------------------------------------------------------------------

#include "Base/Json.h"

namespace rojoRHI::base {

//======================================================================================================================
void appendJsonEscaped(std::string& out, std::string_view text) {
    constexpr char kHexDigits[] = "0123456789abcdef";
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"') {
            out += "\\\"";
        } else if (byte == '\\') {
            out += "\\\\";
        } else if (byte < 0x20) {
            out += "\\u00";
            out += kHexDigits[byte >> 4];
            out += kHexDigits[byte & 0xf];
        } else {
            out += character;
        }
    }
}

//======================================================================================================================
std::string jsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    appendJsonEscaped(out, text);
    return out;
}

} // namespace rojoRHI::base
