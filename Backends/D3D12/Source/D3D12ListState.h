//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ListState.h
/// @brief Declares the D3D12 device's command list pools, one per frame slot.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace rojoRHI {
    struct CommandListImpl;
}

namespace rojoRHI::d3d12 {
    /// The command lists of one frame slot: those taken since the slot was reused come first and the rest wait to be reused.
    struct ListPool
    {
        std::mutex mutex;                                    ///< Guards the pool, because acquisition is free-threaded.
        std::vector<std::unique_ptr<CommandListImpl>> lists; ///< Every list the slot ever needed.
        size_t used = 0;                                     ///< How many of `lists` the open frame has taken.
    };

    /// The command list pools of a device, one per frame slot; a device uses its first `framesInFlight`.
    class ListPools
    {
      public:
        /// Takes a list of frame slot `slot`'s pool, reusing one when the pool has one left, and returns it.
        CommandListImpl& acquire(uint32_t slot);

        /// Returns every list frame slot `slot` took to its pool, once the slot's frame has retired.
        void reset(uint32_t slot);

      private:
        std::array<ListPool, 3> m_slots;
    };
} // namespace rojoRHI::d3d12
