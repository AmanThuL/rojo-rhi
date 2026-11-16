//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4AddressMap.h
/// @brief Declares the map from GPU virtual addresses to the buffers of a Metal 4 device, and the process-wide address reach.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/Addresses.h"

#include <Metal/Metal.hpp>

#include <cstdint>
#include <map>
#include <mutex>
#include <set>

namespace rojoRHI::metal4 {
    /// The buffers of one device ordered by GPU address, so an address decodes to the buffer slot it lies in. A buffer joins
    /// when it is created, is marked when it is destroyed and leaves when its slot retires. Placed buffers may alias, so
    /// several buffers can share a base address or overlap. Every member takes the map's own mutex and calls nothing else, so
    /// a retire hook may call it under a pool's lock. Only Debug checks decode addresses, so the backend maintains the map in
    /// Debug builds only.
    class AddressMap
    {
      public:
        /// Adds `buffer`, of `size` bytes from GPU address `base`, which lives in buffer slot `slot`.
        void insert(uint64_t base, uint64_t size, uint32_t slot, const MTL::Buffer* buffer);

        /// Marks `buffer`, added at `base`, as destroyed: it still decodes until it leaves, but only where no live buffer
        /// holds the same address.
        void markDestroyed(uint64_t base, const MTL::Buffer* buffer);

        /// Removes `buffer`, added at `base`; does nothing when it is not in the map.
        void erase(uint64_t base, const MTL::Buffer* buffer);

        /// Returns the slot of a buffer that holds `address` and the offset of `address` in it, preferring a buffer that is not
        /// destroyed, or `{ 0, 0 }` when no buffer of the map holds it.
        core::DecodedAddr find(uint64_t address) const;

      private:
        /// One buffer of the map.
        struct Entry
        {
            uint64_t size = 0;                   ///< Bytes from the base address.
            uint32_t slot = 0;                   ///< The buffer's slot in the buffer pool.
            const MTL::Buffer* buffer = nullptr; ///< The native buffer, which tells aliased entries apart.
            bool destroyed = false;              ///< True once the buffer is destroyed and until its slot retires.
        };

        mutable std::mutex m_mutex;
        std::multimap<uint64_t, Entry> m_entries; ///< Keyed by base address; equal keys keep their order of insertion.
        /// The size of every entry, whose largest bounds the search below an address.
        std::multiset<uint64_t> m_sizes;
    };

    /// Returns the largest offset `operator+` accepts, `Limits::maxAddrOffset` of the system default device that every device
    /// of the process wraps; zero until a device is created.
    uint64_t maxAddrOffset();

    /// Stores the value `maxAddrOffset` returns; `createDevice` calls it.
    void setMaxAddrOffset(uint64_t value);
} // namespace rojoRHI::metal4
