//----------------------------------------------------------------------------------------------------------------------
/// @file FrameRing.cpp
/// @brief Implements the per-frame-slot growable upload buffer of the ImGui renderer.
//----------------------------------------------------------------------------------------------------------------------
#include "FrameRing.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <utility>

namespace rojoRHI::imgui::detail {
    namespace {
        constexpr uint64_t kMinimumCapacity = 256 * 1024;
        constexpr uint64_t kCapacityGranule = 64 * 1024;

        //==============================================================================================================
        constexpr uint64_t alignUp(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }
    } // namespace

    //==================================================================================================================
    FrameRing::FrameRing(Device device, std::string_view label) : m_device(device), m_label(label)
    {
    }

    //==================================================================================================================
    FrameRing::Allocation FrameRing::allocate(uint64_t size, uint64_t alignment)
    {
        if (m_advance)
        {
            m_current = (m_current + 1) % kFrameSlots;
            m_slots[m_current].used = 0;
            m_advance = false;
        }
        Slot* slot = &m_slots[m_current];
        uint64_t offset = alignUp(slot->used, alignment);
        if (slot->buffer.bits == 0 || offset + size > slot->capacity)
        {
            grow(size);
            slot = &m_slots[m_current];
            offset = 0;
        }
        slot->used = offset + size;
        return Allocation{ .buffer = slot->buffer, .offset = offset, .cpu = slot->cpu + offset, .address = slot->base + offset };
    }

    //==================================================================================================================
    void FrameRing::grow(uint64_t request)
    {
        Slot& slot = m_slots[m_current];
        const uint64_t capacity = std::max({ 2 * slot.capacity, alignUp(request, kCapacityGranule), kMinimumCapacity });
        const std::string label = std::format("{}.frame{}", m_label, m_current);
        Result<Buffer> created = m_device.createBuffer(BufferDesc{ .size = capacity, .memory = Memory::Upload, .usage = BufferUsage::Index, .label = label });
        if (!created)
        {
            std::fprintf(
                stderr,
                "rojoRHI::imgui: creating the %llu-byte frame buffer %s failed: %s\n",
                static_cast<unsigned long long>(capacity),
                label.c_str(),
                created.error().message.c_str()
            );
            std::abort();
        }
        // Data written earlier this frame stays in the old buffer, which lists of this frame still read.
        if (slot.buffer.bits != 0)
        {
            m_retired.push_back(slot.buffer);
        }
        slot.buffer = *created;
        slot.base = m_device.address(slot.buffer);
        slot.cpu = m_device.cpuPointer(slot.buffer);
        slot.capacity = capacity;
        slot.used = 0;
    }

    //==================================================================================================================
    void FrameRing::newFrame()
    {
        for (const Buffer buffer : m_retired)
        {
            m_device.destroy(buffer);
        }
        m_retired.clear();
        m_advance = true;
    }

    //==================================================================================================================
    void FrameRing::destroy()
    {
        newFrame();
        for (Slot& slot : m_slots)
        {
            if (slot.buffer.bits != 0)
            {
                m_device.destroy(slot.buffer);
            }
            slot = Slot{};
        }
    }
} // namespace rojoRHI::imgui::detail
