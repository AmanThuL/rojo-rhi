//----------------------------------------------------------------------------------------------------------------------
/// @file Message.cpp
/// @brief Implements the process-wide message callback and its stderr default.
//----------------------------------------------------------------------------------------------------------------------

#include "Base/Assert.h"
#include "Base/Log.h"

#include <cstdio>
#include <mutex>
#include <string>

namespace rojoRHI {
    namespace {
        std::mutex gSinkMutex;
        MessageCallback gCallback = nullptr;
        void* gUser = nullptr;
        thread_local bool gInsideCallback = false;

        //==============================================================================================================
        const char* severityTag(MessageSeverity severity)
        {
            switch (severity)
            {
            case MessageSeverity::Info: return "info";
            case MessageSeverity::Warning: return "warning";
            case MessageSeverity::Error: return "error";
            }
            return "info";
        }

        //==============================================================================================================
        /// Writes `message` to standard error as "[RHI] [severity] message" and flushes it.
        void writeDefault(MessageSeverity severity, std::string_view message)
        {
            std::string line = "[RHI] [";
            line += severityTag(severity);
            line += "] ";
            line.append(message);
            line += '\n';
            std::fputs(line.c_str(), stderr);
            std::fflush(stderr);
        }

        //==============================================================================================================
        /// Delivers `message` to the installed callback, or to the stderr default, and returns whether a callback received it.
        bool deliverMessage(MessageSeverity severity, std::string_view message)
        {
            MessageCallback callback = nullptr;
            void* user = nullptr;
            {
                const std::lock_guard<std::mutex> lock(gSinkMutex);
                callback = gCallback;
                user = gUser;
            }

            // A message emitted from inside the callback goes to the default sink: routing it back would re-enter the callback.
            if (callback != nullptr && !gInsideCallback)
            {
                gInsideCallback = true;
                callback(severity, message, user);
                gInsideCallback = false;
                return true;
            }
            writeDefault(severity, message);
            return false;
        }
    } // namespace

    //==================================================================================================================
    void setMessageCallback(MessageCallback callback, void* user)
    {
        ROJORHI_VU(!base::insideMessageCallback(), "VU-rojo-core-0008", "setMessageCallback was called from inside a message callback");
        const std::lock_guard<std::mutex> lock(gSinkMutex);
        gCallback = callback;
        gUser = user;
    }

    namespace base {
        //==============================================================================================================
        void emitMessage(MessageSeverity severity, std::string_view message)
        {
            deliverMessage(severity, message);
        }

        //==============================================================================================================
        void emitFatal(std::string_view message)
        {
            // A callback may keep the message where an abort loses it, so standard error gets it too.
            if (deliverMessage(MessageSeverity::Error, message))
            {
                writeDefault(MessageSeverity::Error, message);
            }
        }

        //==============================================================================================================
        bool insideMessageCallback()
        {
            return gInsideCallback;
        }
    } // namespace base
} // namespace rojoRHI
