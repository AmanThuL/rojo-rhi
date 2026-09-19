//----------------------------------------------------------------------------------------------------------------------
/// @file Log.h
/// @brief Declares the RHI-private logging macros over the public message sink.
//----------------------------------------------------------------------------------------------------------------------

#pragma once
#include "RHI/Message.h"

#include <format>
#include <string_view>

namespace lmx::rhi::base {

/// Delivers one already-formatted message to the installed sink, or to the stderr default.
void emitMessage(MessageSeverity severity, std::string_view message);

} // namespace lmx::rhi::base

/// Emits an informational RHI log message.
#define LMX_LOG_INFO(...)                                                                          \
    ::lmx::rhi::base::emitMessage(::lmx::rhi::MessageSeverity::Info, std::format(__VA_ARGS__))
/// Emits a warning RHI log message.
#define LMX_LOG_WARN(...)                                                                          \
    ::lmx::rhi::base::emitMessage(::lmx::rhi::MessageSeverity::Warning, std::format(__VA_ARGS__))
/// Emits an error RHI log message.
#define LMX_LOG_ERROR(...)                                                                         \
    ::lmx::rhi::base::emitMessage(::lmx::rhi::MessageSeverity::Error, std::format(__VA_ARGS__))
