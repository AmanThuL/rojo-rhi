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
    // Placed buffers may alias, so several buffers can share a base address or overlap. Every member takes only the
    // map's own mutex and calls nothing else, so a retire hook may call it under a pool's lock. Maintained in Debug
    // builds only.
    class AddressMap
    {
      public:
        void insert(uint64_t base, uint64_t size, uint32_t slot, const MTL::Buffer* buffer);

        // It still decodes until it leaves, but only where no live buffer holds the same address.
        void markDestroyed(uint64_t base, const MTL::Buffer* buffer);

        // A buffer not in the map is ignored.
        void erase(uint64_t base, const MTL::Buffer* buffer);

        // Prefers a buffer that is not destroyed; `{ 0, 0 }` when no buffer holds `address`.
        core::DecodedAddr find(uint64_t address) const;

      private:
        struct Entry
        {
            uint64_t size = 0;
            uint32_t slot = 0;
            const MTL::Buffer* buffer = nullptr; // Tells aliased entries apart.
            bool destroyed = false;              // Until its slot retires.
        };

        mutable std::mutex m_mutex;
        std::multimap<uint64_t, Entry> m_entries; // Equal keys keep their order of insertion.
        // The largest bounds the search below an address.
        std::multiset<uint64_t> m_sizes;
    };

    // `Limits::maxAddrOffset` of the system default device, which every device of the process wraps; zero until a
    // device is created.
    uint64_t maxAddrOffset();

    void setMaxAddrOffset(uint64_t value);
} // namespace rojoRHI::metal4
