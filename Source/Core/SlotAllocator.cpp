//----------------------------------------------------------------------------------------------------------------------
/// @file SlotAllocator.cpp
/// @brief Implements the allocator of heap slots with deferred reuse.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/SlotAllocator.h"

namespace rojoRHI::core {
    //==================================================================================================================
    std::optional<uint32_t> SlotAllocator::allocate(uint32_t capacity, uint64_t owner)
    {
        std::lock_guard lock(m_mutex);
        uint32_t index = 0;
        if (!m_free.empty())
        {
            index = m_free.back();
            m_free.pop_back();
        }
        else if (m_slots.size() < capacity)
        {
            index = static_cast<uint32_t>(m_slots.size());
            m_slots.emplace_back();
        }
        else
        {
            return std::nullopt;
        }
        m_slots[index] = Slot{ SlotState::Assigned, owner };
        return index;
    }

    //==================================================================================================================
    std::optional<uint32_t> SlotAllocator::allocateRange(uint32_t capacity, uint32_t count, uint64_t owner)
    {
        std::lock_guard lock(m_mutex);
        const uint32_t handedOut = static_cast<uint32_t>(m_slots.size());
        uint32_t start = 0;
        uint32_t run = 0;
        for (uint32_t i = 0; i < handedOut && run < count; ++i)
        {
            if (m_slots[i].state != SlotState::Free)
            {
                run = 0;
                continue;
            }
            start = run == 0 ? i : start;
            ++run;
        }
        if (run < count)
        {
            // Every slot from `handedOut` on was never handed out, so the run continues into them up to the capacity.
            start = run == 0 ? handedOut : start;
            if (start > capacity || count > capacity - start)
            {
                return std::nullopt;
            }
            m_slots.resize(std::max<size_t>(m_slots.size(), uint64_t{ start } + count));
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            m_slots[start + i] = Slot{ SlotState::Assigned, owner, i == 0 ? count : 0, true };
        }
        std::erase_if(m_free, [&](uint32_t index) { return index >= start && index - start < count; });
        return start;
    }

    //==================================================================================================================
    bool SlotAllocator::holdsRange(uint32_t base, uint32_t count)
    {
        std::lock_guard lock(m_mutex);
        return count != 0 && base < m_slots.size() && m_slots[base].state == SlotState::Assigned && m_slots[base].rangeCount == count;
    }

    //==================================================================================================================
    std::optional<uint64_t> SlotAllocator::replaceOwner(uint32_t index, uint64_t owner)
    {
        std::lock_guard lock(m_mutex);
        if (index >= m_slots.size() || m_slots[index].state != SlotState::Assigned || !m_slots[index].inRange)
        {
            return std::nullopt;
        }
        return std::exchange(m_slots[index].owner, owner);
    }

    //==================================================================================================================
    uint64_t SlotAllocator::retire(uint64_t upToFrame)
    {
        std::lock_guard lock(m_mutex);
        uint64_t singles = 0;
        std::erase_if(m_pending, [&](const Pending& entry) {
            if (entry.frame > upToFrame)
            {
                return false;
            }
            for (uint32_t i = 0; i < entry.count; ++i)
            {
                m_slots[entry.index + i] = Slot{};
                m_free.push_back(entry.index + i);
            }
            singles += entry.range ? 0 : 1;
            return true;
        });
        return singles;
    }

    //==================================================================================================================
    SlotState SlotAllocator::state(uint32_t index)
    {
        std::lock_guard lock(m_mutex);
        return index < m_slots.size() ? m_slots[index].state : SlotState::Free;
    }

    //==================================================================================================================
    size_t SlotAllocator::assignedCount()
    {
        std::lock_guard lock(m_mutex);
        return static_cast<size_t>(std::count_if(m_slots.begin(), m_slots.end(), [](const Slot& slot) { return slot.state == SlotState::Assigned; }));
    }
} // namespace rojoRHI::core
