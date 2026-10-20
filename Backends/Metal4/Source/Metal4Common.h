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
    /// An autorelease pool for the scope that holds it. Every entry that calls an autoreleasing factory opens one for its body,
    /// and every function that releases Metal objects opens one before it releases them.
    class AutoreleasePool
    {
      public:
        AutoreleasePool();
        ~AutoreleasePool();

        AutoreleasePool(const AutoreleasePool&) = delete;
        AutoreleasePool& operator=(const AutoreleasePool&) = delete;

      private:
        void* m_boundary; ///< The token `objc_autoreleasePoolPush` returned.
    };

    /// The autorelease pools the calling thread has opened through `AutoreleasePool` and `CommandPool`. A recording entry that
    /// opens no pool leaves the count unchanged. The count is the thread's own, so counting touches no shared cache line.
    inline uint64_t& poolsOpened()
    {
        static thread_local uint64_t opened = 0;
        return opened;
    }

    /// The pools the calling thread has opened through `CommandPool` alone: what the native commands open.
    inline uint64_t& commandPoolsOpened()
    {
        static thread_local uint64_t opened = 0;
        return opened;
    }

    //==================================================================================================================
    inline AutoreleasePool::AutoreleasePool() : m_boundary(objc_autoreleasePoolPush())
    {
        ++poolsOpened();
    }

    //==================================================================================================================
    inline AutoreleasePool::~AutoreleasePool()
    {
        objc_autoreleasePoolPop(m_boundary);
    }

    /// An autorelease pool for one native command, opened only when `open` is true. The Metal validation layer autoreleases
    /// objects inside the commands it checks, and without it those commands autorelease nothing, so a device passes whether
    /// its commands may autorelease (`DeviceImpl::commandPools`, `commandsMayAutorelease`). With `open` false the object is
    /// one untaken branch. Entries that call an autoreleasing factory use `AutoreleasePool`, which always opens one.
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
        void* m_boundary = nullptr; ///< The token `objc_autoreleasePoolPush` returned; null when no pool was opened.
    };

    /// Returns whether a native command may autorelease, which is every case but one: the device's class name is positively a
    /// bare driver class, whose names start with "AGX" on Apple silicon. The validation layer replaces the device with a debug
    /// class ("MTLDebugDevice"), and any other name, a capture or shader-validation wrapper or none at all, is treated as a
    /// wrapper too, so detection fails closed and opens the pools. It reads the object, never the environment.
    bool commandsMayAutorelease(const MTL::Device* device);

    /// Returns `text` as an autoreleased string, so the caller holds a pool. The text is copied first, because a view is not
    /// NUL-terminated.
    NS::String* makeString(std::string_view text);

    /// Returns the UTF-8 text of `string`, or an empty string for null.
    std::string toStdString(NS::String* string);

    /// Returns the localized description of `error` with its domain and code, or a fixed text for null.
    std::string describe(NS::Error* error);

    /// The longest single native wait on an event, in milliseconds; a longer wait is made of slices of this length.
    constexpr uint64_t kWaitSliceMilliseconds = 10'000;

    /// Blocks until `event` reaches `value`, waiting in slices of `kWaitSliceMilliseconds`. After each slice that passes
    /// without it, it logs a warning naming the event as `what` followed by the quoted `label`, or the event's own label when
    /// `label` is empty, and `value`; nothing is read or formatted before that.
    void waitUntilSignaled(MTL::SharedEvent* event, uint64_t value, std::string_view what, std::string_view label);
} // namespace rojoRHI::metal4
