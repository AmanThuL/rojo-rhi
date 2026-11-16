//----------------------------------------------------------------------------------------------------------------------
/// @file SlotAllocator.cpp
/// @brief Implements the allocator of heap slots with deferred reuse.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/SlotAllocator.h"

namespace rojoRHI::core {
    //==================================================================================================================
    void SlotAllocator::reserveLeading(uint32_t count)
    {
        std::lock_guard lock(m_mutex);
        ROJORHI_INTERNAL_ASSERT(m_slots.empty() && m_pending.empty(), "slots reserved on an allocator that has handed some out");
        m_slots.assign(count, Slot{ SlotState::Reserved, 0, 0, false, nullptr });
        m_reserved = count;
    }

    //==================================================================================================================
    std::optional<uint32_t> SlotAllocator::allocate(uint32_t capacity, uint64_t owner, void* native)
    {
        std::lock_guard lock(m_mutex);
        uint32_t index = 0;
        if (!m_free.empty())
        {
            index = m_free.back();
            m_free.pop_back();
        }
        else if (m_slots.size() < uint64_t{ m_reserved } + capacity)
        {
            index = static_cast<uint32_t>(m_slots.size());
            m_slots.emplace_back();
        }
        else
        {
            return std::nullopt;
        }
        m_slots[index] = Slot{ SlotState::Assigned, owner, 0, false, native };
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
            const uint64_t limit = uint64_t{ m_reserved } + capacity;
            if (start > limit || count > limit - start)
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
    uint64_t SlotAllocator::retire(uint64_t upToFrame)
    {
        return retire(upToFrame, [](void*) { ROJORHI_INTERNAL_ASSERT(false, "a slot payload was retired with no one to release it"); });
    }

    //==================================================================================================================
    bool SlotAllocator::empty()
    {
        std::lock_guard lock(m_mutex);
        return m_slots.size() == m_reserved && m_pending.empty();
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
