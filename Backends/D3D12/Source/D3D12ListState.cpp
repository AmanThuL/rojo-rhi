//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ListState.cpp
/// @brief Implements the D3D12 command list pools: each frame slot hands out the lists it keeps and grows when it runs out.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12ListState.h"

#include "Base/Assert.h"
#include "Core/Counters.h"
#include "D3D12Device.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace rojoRHI::d3d12 {
    //==================================================================================================================
    CommandListImpl& ListPools::acquire(uint32_t slot, core::DeviceCounts& counts)
    {
        ROJORHI_INTERNAL_ASSERT(slot < m_slots.size(), "a frame slot beyond the three a device may use has no list pool");
        ListPool& pool = m_slots[slot];
        std::lock_guard lock(pool.mutex);
        if (pool.used == pool.lists.size())
        {
            pool.lists.push_back(std::make_unique<CommandListImpl>());
            core::DeviceCounts::add(counts.cpuAllocations);
        }
        return *pool.lists[pool.used++];
    }

    //==================================================================================================================
    void ListPools::reset(uint32_t slot)
    {
        ROJORHI_INTERNAL_ASSERT(slot < m_slots.size(), "a frame slot beyond the three a device may use has no list pool");
        ListPool& pool = m_slots[slot];
        std::lock_guard lock(pool.mutex);
        pool.used = 0;
    }
} // namespace rojoRHI::d3d12
