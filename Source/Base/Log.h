//----------------------------------------------------------------------------------------------------------------------
/// @file Log.h
/// @brief Declares the private logging macros and the delivery path to the public message callback.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <format>
#include <string_view>

namespace rojoRHI::base {
    /// Delivers one formatted message to the installed callback, or to the stderr default.
    void emitMessage(MessageSeverity severity, std::string_view message);

    /// Delivers one formatted message at error severity that the caller aborts after: to the installed callback and then to
    /// stderr as well, or to the stderr default alone, so an abort never ends the process without its reason on stderr.
    void emitFatal(std::string_view message);

    /// Returns true on a thread currently inside a message callback; VU-rojo-core-0008 reads it.
    bool insideMessageCallback();
} // namespace rojoRHI::base

/// Emits an informational message.
#define ROJORHI_LOG_INFO(...) ::rojoRHI::base::emitMessage(::rojoRHI::MessageSeverity::Info, std::format(__VA_ARGS__))
/// Emits a warning message.
#define ROJORHI_LOG_WARN(...) ::rojoRHI::base::emitMessage(::rojoRHI::MessageSeverity::Warning, std::format(__VA_ARGS__))
/// Emits an error message.
#define ROJORHI_LOG_ERROR(...) ::rojoRHI::base::emitMessage(::rojoRHI::MessageSeverity::Error, std::format(__VA_ARGS__))
/// Emits an error message the caller aborts after, which reaches stderr even when a callback is installed.
#define ROJORHI_LOG_FATAL(...) ::rojoRHI::base::emitFatal(std::format(__VA_ARGS__))
