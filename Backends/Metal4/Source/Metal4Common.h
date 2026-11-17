//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Common.h
/// @brief Declares the helpers every Metal 4 backend file shares: autorelease pools, strings and event waits.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// The Objective-C runtime's pool entries, which clang emits for @autoreleasepool: a push returns a boundary token and a pop
// releases every object autoreleased since it. A pushed pool allocates nothing, unlike an NSAutoreleasePool object.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* boundary);

namespace rojoRHI::metal4 {
    // Every entry that calls an autoreleasing factory opens one for its body, and every function that releases Metal
    // objects opens one before it releases them.
    class AutoreleasePool
    {
      public:
        AutoreleasePool();
        ~AutoreleasePool();

        AutoreleasePool(const AutoreleasePool&) = delete;
        AutoreleasePool& operator=(const AutoreleasePool&) = delete;

      private:
        void* m_boundary;
    };

    // Per thread, so counting touches no shared cache line. A recording entry that opens no pool leaves the count
    // unchanged.
    inline uint64_t& poolsOpened()
    {
        static thread_local uint64_t opened = 0;
        return opened;
    }

    inline uint64_t& commandPoolsOpened()
    {
        static thread_local uint64_t opened = 0;
        return opened;
    }

    inline AutoreleasePool::AutoreleasePool() : m_boundary(objc_autoreleasePoolPush())
    {
        ++poolsOpened();
    }

    inline AutoreleasePool::~AutoreleasePool()
    {
        objc_autoreleasePoolPop(m_boundary);
    }

    // Opened only when `open` is true. The Metal validation layer autoreleases inside the commands it checks; without
    // it they autorelease nothing, so a device passes whether its commands may (`commandsMayAutorelease`). Entries that
    // call an autoreleasing factory use `AutoreleasePool`, which always opens one.
    class CommandPool
    {
      public:
        explicit CommandPool(bool open)
        {
            if (open)
            {
                m_boundary = objc_autoreleasePoolPush();
                ++poolsOpened();
                ++commandPoolsOpened();
            }
        }

        ~CommandPool()
        {
            if (m_boundary != nullptr)
            {
                objc_autoreleasePoolPop(m_boundary);
            }
        }

        CommandPool(const CommandPool&) = delete;
        CommandPool& operator=(const CommandPool&) = delete;

      private:
        void* m_boundary = nullptr; // Null when no pool was opened.
    };

    // True unless the device's class name is positively a bare driver class ("AGX" on Apple silicon). The validation
    // layer's MTLDebugDevice, a capture or shader-validation wrapper, or no name at all count as wrappers, so detection
    // fails closed. It reads the object, never the environment.
    bool commandsMayAutorelease(const MTL::Device* device);

    // Autoreleased, so the caller holds a pool. The text is copied first, because a view is not NUL-terminated.
    NS::String* makeString(std::string_view text);

    std::string toStdString(NS::String* string);

    std::string describe(NS::Error* error);

    // A longer wait is made of slices of this length.
    constexpr uint64_t kWaitSliceMilliseconds = 10'000;

    // Logs a warning after each slice that passes, with the event's own label when `label` is empty; nothing is read or
    // formatted before that.
    void waitUntilSignaled(MTL::SharedEvent* event, uint64_t value, std::string_view what, std::string_view label);
} // namespace rojoRHI::metal4
