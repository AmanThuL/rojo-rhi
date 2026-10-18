//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Common.h
/// @brief Declares the helpers every Metal 4 backend file shares: aborts, autorelease pools, strings and event waits.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace rojoRHI::metal4 {
    /// Logs that the Metal 4 backend does not implement `entry` and aborts, in every build: an entry is never silently empty.
    [[noreturn]] void notImplemented(std::string_view entry);

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
        NS::AutoreleasePool* m_pool = nullptr;
    };

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
