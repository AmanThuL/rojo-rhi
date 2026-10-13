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

        //==============================================================================================================
        bool insideMessageCallback()
        {
            return gInsideCallback;
        }
    } // namespace base
} // namespace rojoRHI
