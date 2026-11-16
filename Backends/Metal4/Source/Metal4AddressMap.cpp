//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4AddressMap.cpp
/// @brief Implements the map from GPU virtual addresses to the buffers of a Metal 4 device.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4AddressMap.h"

#include "Core/Addresses.h"

#include <Metal/Metal.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>

namespace rojoRHI::metal4 {
    namespace {
        /// The value `maxAddrOffset` returns.
        std::atomic<uint64_t> g_maxAddrOffset = 0;
    } // namespace

    //==================================================================================================================
    void AddressMap::insert(uint64_t base, uint64_t size, uint32_t slot, const MTL::Buffer* buffer)
    {
        std::lock_guard lock(m_mutex);
        m_entries.emplace(base, Entry{ size, slot, buffer, false });
        m_sizes.insert(size);
    }

    //==================================================================================================================
    void AddressMap::markDestroyed(uint64_t base, const MTL::Buffer* buffer)
    {
        std::lock_guard lock(m_mutex);
        const auto [first, last] = m_entries.equal_range(base);
        for (auto it = first; it != last; ++it)
        {
            if (it->second.buffer == buffer)
            {
                it->second.destroyed = true;
                return;
            }
        }
    }

    //==================================================================================================================
    void AddressMap::erase(uint64_t base, const MTL::Buffer* buffer)
    {
        std::lock_guard lock(m_mutex);
        const auto [first, last] = m_entries.equal_range(base);
        for (auto it = first; it != last; ++it)
        {
            if (it->second.buffer == buffer)
            {
                m_sizes.erase(m_sizes.find(it->second.size));
                m_entries.erase(it);
                return;
            }
        }
    }

    //==================================================================================================================
    core::DecodedAddr AddressMap::find(uint64_t address) const
    {
        std::lock_guard lock(m_mutex);
        if (m_sizes.empty())
        {
            return core::DecodedAddr{ 0, 0 };
        }
        const uint64_t largest = *m_sizes.rbegin();
        std::optional<core::DecodedAddr> destroyed;
        // Walks down from the highest base at or below the address. No entry whose base lies the largest size of the map or
        // more below the address can hold it, which ends the walk.
        for (auto it = m_entries.upper_bound(address); it != m_entries.begin();)
        {
            --it;
            const uint64_t offset = address - it->first;
            if (offset >= largest)
            {
                break;
            }
            if (offset < it->second.size)
            {
                if (!it->second.destroyed)
                {
                    return core::DecodedAddr{ it->second.slot, offset };
                }
                if (!destroyed)
                {
                    destroyed = core::DecodedAddr{ it->second.slot, offset };
                }
            }
        }
        return destroyed.value_or(core::DecodedAddr{ 0, 0 });
    }

    //==================================================================================================================
    uint64_t maxAddrOffset()
    {
        return g_maxAddrOffset.load(std::memory_order_relaxed);
    }

    //==================================================================================================================
    void setMaxAddrOffset(uint64_t value)
    {
        g_maxAddrOffset.store(value, std::memory_order_relaxed);
    }
} // namespace rojoRHI::metal4
