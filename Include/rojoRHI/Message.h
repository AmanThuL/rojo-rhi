//----------------------------------------------------------------------------------------------------------------------
/// @file Message.h
/// @brief Declares the process-wide RHI diagnostic message sink.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string_view>

namespace rojoRHI {

/// Classifies the urgency of an RHI diagnostic message.
enum class MessageSeverity : uint8_t {
    Info,    ///< Routine progress or configuration detail.
    Warning, ///< A recoverable condition worth reporting.
    Error,   ///< A failure, including the text of a failed contract check.
};

/// Receives one RHI diagnostic message. `message` is valid only for the duration of the call.
using MessageCallback = void (*)(MessageSeverity severity, std::string_view message, void* user);

/// Installs the process-wide sink; nullptr restores the stderr default. Thread-safe.
void setMessageCallback(MessageCallback callback, void* user);

} // namespace rojoRHI
