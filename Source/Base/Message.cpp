//----------------------------------------------------------------------------------------------------------------------
/// @file Message.cpp
/// @brief Implements the process-wide RHI diagnostic message sink and its stderr default.
//----------------------------------------------------------------------------------------------------------------------

#include "Base/Log.h"

#include <cstdio>
#include <mutex>
#include <string>

namespace rojoRHI {
namespace {

std::mutex gSinkMutex;
MessageCallback gCallback = nullptr;
void* gUser = nullptr;

//======================================================================================================================
/// The stable lowercase tag written by the stderr default for each severity.
const char* severityTag(MessageSeverity severity) {
    switch (severity) {
    case MessageSeverity::Info:
        return "info";
    case MessageSeverity::Warning:
        return "warning";
    case MessageSeverity::Error:
        return "error";
    }
    return "info";
}

} // namespace

//======================================================================================================================
void setMessageCallback(MessageCallback callback, void* user) {
    const std::lock_guard<std::mutex> lock(gSinkMutex);
    gCallback = callback;
    gUser = user;
}

namespace base {

//======================================================================================================================
void emitMessage(MessageSeverity severity, std::string_view message) {
    MessageCallback callback = nullptr;
    void* user = nullptr;
    {
        const std::lock_guard<std::mutex> lock(gSinkMutex);
        callback = gCallback;
        user = gUser;
    }
    if (callback != nullptr) {
        callback(severity, message, user);
        return;
    }

    std::string line = "[RHI] [";
    line += severityTag(severity);
    line += "] ";
    line.append(message);
    line += '\n';
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

} // namespace base
} // namespace rojoRHI
