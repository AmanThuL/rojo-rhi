//----------------------------------------------------------------------------------------------------------------------
/// @file FrameArena.cpp
/// @brief Implements the frame arena's page claiming and rewinding.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/FrameArena.h"

#include "Base/Assert.h"
#include "Base/Log.h"

#include <algorithm>
#include <cstdlib>
#include <format>

namespace rojoRHI::core {
    //==================================================================================================================
    FrameArena::FrameArena(uint32_t slots) : m_slots(slots)
    {
    }

    //==================================================================================================================
    ArenaPage FrameArena::claim(uint32_t slot, uint64_t minSize, uint64_t maxPageBytes, ArenaPageSource source, void* context)
    {
        std::lock_guard lock(m_mutex);
        ROJORHI_INTERNAL_ASSERT(slot < m_slots.size(), "a frame slot outside the device's slots was used");
        std::vector<Entry>& entries = m_slots[slot];
        for (Entry& entry : entries)
        {
            if (!entry.claimed && entry.page.size >= minSize)
            {
                entry.claimed = true;
                return entry.page;
            }
        }
        if (minSize > maxPageBytes)
        {
            base::emitMessage(
                MessageSeverity::Error, std::format("the frame arena cannot hold {} bytes of frame data: a page is at most {} bytes", minSize, maxPageBytes)
            );
            std::abort();
        }
        uint64_t pageSize = std::max(kFrameArenaPageBytes, minSize);
        if (pageSize > maxPageBytes)
        {
            pageSize = minSize;
        }
        Result<ArenaPage> created = source(context, pageSize);
        if (!created)
        {
            base::emitMessage(MessageSeverity::Error, std::format("the frame arena could not grow by {} bytes: {}", pageSize, created.error().message));
            std::abort();
        }
        m_growths.fetch_add(1, std::memory_order_relaxed);
        entries.push_back(Entry{ *created, true });
        return *created;
    }

    //==================================================================================================================
    void FrameArena::rewind(uint32_t slot)
    {
        std::lock_guard lock(m_mutex);
        ROJORHI_INTERNAL_ASSERT(slot < m_slots.size(), "a frame slot outside the device's slots was rewound");
        for (Entry& entry : m_slots[slot])
        {
            entry.claimed = false;
        }
    }

    //==================================================================================================================
    std::vector<Buffer> FrameArena::takePages()
    {
        std::lock_guard lock(m_mutex);
        std::vector<Buffer> taken;
        for (std::vector<Entry>& entries : m_slots)
        {
            for (const Entry& entry : entries)
            {
                taken.push_back(entry.page.buffer);
            }
            entries.clear();
        }
        return taken;
    }

    //==================================================================================================================
    std::vector<Buffer> FrameArena::pageBuffers() const
    {
        std::lock_guard lock(m_mutex);
        std::vector<Buffer> buffers;
        for (const std::vector<Entry>& entries : m_slots)
        {
            for (const Entry& entry : entries)
            {
                buffers.push_back(entry.page.buffer);
            }
        }
        return buffers;
    }

    //==================================================================================================================
    uint64_t FrameArena::growths() const
    {
        return m_growths.load(std::memory_order_relaxed);
    }
} // namespace rojoRHI::core
