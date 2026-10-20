//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Common.cpp
/// @brief Implements the helpers every Metal 4 backend file shares.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Common.h"

#include "Base/Log.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <objc/runtime.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

namespace rojoRHI::metal4 {
    //==================================================================================================================
    AutoreleasePool::AutoreleasePool() : m_pool(NS::AutoreleasePool::alloc()->init())
    {
        ++poolsOpened();
    }

    //==================================================================================================================
    AutoreleasePool::~AutoreleasePool()
    {
        m_pool->release();
    }

    //==================================================================================================================
    bool commandsMayAutorelease(const MTL::Device* device)
    {
        const char* name = object_getClassName(reinterpret_cast<id>(const_cast<MTL::Device*>(device)));
        return name == nullptr || std::strncmp(name, "AGX", 3) != 0;
    }

    //==================================================================================================================
    NS::String* makeString(std::string_view text)
    {
        const std::string owned(text);
        return NS::String::string(owned.c_str(), NS::UTF8StringEncoding);
    }

    //==================================================================================================================
    std::string toStdString(NS::String* string)
    {
        if (string == nullptr)
        {
            return {};
        }
        const char* utf8 = string->utf8String();
        return utf8 != nullptr ? std::string(utf8) : std::string();
    }

    //==================================================================================================================
    std::string describe(NS::Error* error)
    {
        if (error == nullptr)
        {
            return "no error description";
        }
        return std::format("{} ({} {})", toStdString(error->localizedDescription()), toStdString(error->domain()), error->code());
    }

    //==================================================================================================================
    void waitUntilSignaled(MTL::SharedEvent* event, uint64_t value, std::string_view what, std::string_view label)
    {
        uint64_t waitedSeconds = 0;
        while (!event->waitUntilSignaledValue(value, kWaitSliceMilliseconds))
        {
            waitedSeconds += kWaitSliceMilliseconds / 1'000;
            if (!label.empty())
            {
                ROJORHI_LOG_WARN("still waiting after {} s for {} \"{}\" to reach {}", waitedSeconds, what, label, value);
                continue;
            }
            // The event's own label is read only for a warning.
            const AutoreleasePool pool;
            ROJORHI_LOG_WARN("still waiting after {} s for {} \"{}\" to reach {}", waitedSeconds, what, toStdString(event->label()), value);
        }
    }
} // namespace rojoRHI::metal4
