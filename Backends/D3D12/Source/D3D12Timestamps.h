//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Timestamps.h
/// @brief Declares the D3D12 device's timestamp state, each command list's timestamps and the scope-timing hook.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/ScopeRecord.h"

#include <cstdint>
#include <span>

namespace rojoRHI {
    struct CommandListImpl;
    struct DeviceImpl;
} // namespace rojoRHI

namespace rojoRHI::d3d12 {
    /// The device-wide timestamp state, created with the device and fixed for its life. It holds nothing yet, so every scope
    /// duration stays zero.
    class Timestamps
    {
      public:
        /// Creates the state on `impl`'s device; makes nothing and succeeds.
        Result<void> create(DeviceImpl& impl);

        /// Releases what `create` made; nothing.
        void release();
    };

    /// The timestamps one command list records, its own so recording takes no lock. It holds nothing yet.
    struct ListTimestamps
    {
        /// Forgets the previous recording's timestamps; called at each acquisition, after the native list's `Reset`.
        void reset();

        /// Releases the native objects the list's timestamps hold; nothing.
        void release();
    };

    /// Records the native commands that make `list`'s timestamps readable once it completes; called by `CommandList::end`
    /// before `Close`. It records nothing yet.
    void resolveTimestamps(CommandListImpl& list);

    /// The `scopeTimes` retire hook: fills the durations of the scopes the frame slot `slot` recorded on one queue. `context`
    /// is the `DeviceImpl`. It leaves every duration zero.
    void scopeTimes(void* context, uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report);
} // namespace rojoRHI::d3d12
