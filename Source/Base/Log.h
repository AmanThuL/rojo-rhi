//----------------------------------------------------------------------------------------------------------------------
/// @file Log.h
/// @brief Declares the private logging macros and the delivery path to the public message callback.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <format>
#include <string_view>

namespace rojoRHI::base {
    void emitMessage(MessageSeverity severity, std::string_view message);

    // Reaches stderr even when a callback is installed, so an abort that follows never loses its reason.
    void emitFatal(std::string_view message);

    // Per thread; VU-rojo-core-0008 reads it.
    bool insideMessageCallback();
} // namespace rojoRHI::base

#define ROJORHI_LOG_INFO(...) ::rojoRHI::base::emitMessage(::rojoRHI::MessageSeverity::Info, std::format(__VA_ARGS__))
#define ROJORHI_LOG_WARN(...) ::rojoRHI::base::emitMessage(::rojoRHI::MessageSeverity::Warning, std::format(__VA_ARGS__))
#define ROJORHI_LOG_ERROR(...) ::rojoRHI::base::emitMessage(::rojoRHI::MessageSeverity::Error, std::format(__VA_ARGS__))
#define ROJORHI_LOG_FATAL(...) ::rojoRHI::base::emitFatal(std::format(__VA_ARGS__))
