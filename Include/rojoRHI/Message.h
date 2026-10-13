//----------------------------------------------------------------------------------------------------------------------
/// @file Message.h
/// @brief Declares the process-wide diagnostic message callback and its severities.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string_view>

namespace rojoRHI {
    /// Classifies the urgency of a diagnostic message.
    enum class MessageSeverity : uint8_t
    {
        Info,    ///< Routine progress or configuration detail.
        Warning, ///< A recoverable condition worth reporting.
        Error,   ///< A failure, including the text of a violated valid-usage rule.
    };

    /// Receives one diagnostic message; `message` is valid only for the duration of the call and `user` is the pointer
    /// given to setMessageCallback. It may run on any thread, so it must be thread-safe, and it must return normally. It
    /// calls no RojoRHI entry point (VU-rojo-core-0008).
    using MessageCallback = void (*)(MessageSeverity severity, std::string_view message, void* user);

    /// Installs the process-wide message sink; `nullptr` restores the default, which writes each message to stderr.
    /// Free-threaded. The new callback receives every message emitted after the call returns. A previously installed
    /// callback may still be running on another thread when this returns, so its `user` must outlive that call.
    void setMessageCallback(MessageCallback callback, void* user);
} // namespace rojoRHI
