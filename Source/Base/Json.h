//----------------------------------------------------------------------------------------------------------------------
/// @file Json.h
/// @brief Declares JSON string-content escaping without document framing.
//----------------------------------------------------------------------------------------------------------------------

#pragma once
#include <string>
#include <string_view>

namespace rojoRHI::base {

/// Appends JSON string contents, escaping quotes, backslashes and controls as lowercase Unicode
/// escapes. Preserves all other bytes, including UTF-8. Adds no quotes, object or array framing.
/// `text` must not alias `out`, which may reallocate while appending.
void appendJsonEscaped(std::string& out, std::string_view text);

/// Returns escaped JSON string contents under appendJsonEscaped's byte contract, without quotes.
std::string jsonEscape(std::string_view text);

} // namespace rojoRHI::base
