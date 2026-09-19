//----------------------------------------------------------------------------------------------------------------------
/// @file Log.h
/// @brief Declares the RHI-private logging macros over the public message sink.
//----------------------------------------------------------------------------------------------------------------------

#pragma once
#include <rojoRHI/Message.h>

#include <format>
#include <string_view>

namespace rojoRHI::base {

/// Delivers one already-formatted message to the installed sink, or to the stderr default.
void emitMessage(MessageSeverity severity, std::string_view message);

} // namespace rojoRHI::base

/// Emits an informational RHI log message.
#define ROJORHI_LOG_INFO(...)                                                                          \
    ::rojoRHI::base::emitMessage(::rojoRHI::MessageSeverity::Info, std::format(__VA_ARGS__))
/// Emits a warning RHI log message.
#define ROJORHI_LOG_WARN(...)                                                                          \
    ::rojoRHI::base::emitMessage(::rojoRHI::MessageSeverity::Warning, std::format(__VA_ARGS__))
/// Emits an error RHI log message.
#define ROJORHI_LOG_ERROR(...)                                                                         \
    ::rojoRHI::base::emitMessage(::rojoRHI::MessageSeverity::Error, std::format(__VA_ARGS__))
